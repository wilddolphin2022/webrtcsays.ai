#include "client.h"
#include "wsock.h"
#include "direct.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <utility>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <vector>
#include <json/json.h>
#include "rtc_base/thread.h"
#include <ifaddrs.h>
#include <net/if.h>
#include <regex>
#include "status.h"
#include "video.h"
#include "pc/video_track_source.h"

// Forward declaration from peer.cc so we can create a camera source here.
rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
CreateCameraVideoSource(class DirectPeer*, const Options&);

// DirectCallerClient Implementation

DirectCallerClient::DirectCallerClient(const Options& opts)
    : DirectCaller(opts), initialized_(false) {
    resolved_target_port_ = 0;   
    if (!rtc::Thread::Current()) {
        rtc::ThreadManager::Instance()->WrapCurrentThread();
    }
    owner_thread_ = rtc::Thread::Current();
    signaling_client_ = std::make_unique<DirectClient>(opts);
    // Busy when caller already in an active call
    signaling_client_->setIsBusyCallback([this]() {
        // Only consider busy if there's an active call with remote description set
        bool has_pc = this->peer_connection() != nullptr;
        bool has_remote_desc = has_pc && this->peer_connection()->remote_description() != nullptr;
        bool active_call = has_pc && has_remote_desc;
        return active_call;
    });

    // Set up WAITING callback immediately in constructor
    signaling_client_->setWaitingReceivedCallback([this](const std::string&) {
        APP_LOG(AS_INFO) << "DirectCallerClient received WAITING message, checking initialization";
        // Ensure DirectCallerClient is properly initialized before calling Start()
        if (!initialized_) {
            if (!this->Initialize()) {
                APP_LOG(AS_ERROR) << "DirectCallerClient initialization failed, cannot start";
                return;
            }
        }
        
        // Post Start() to signaling thread to avoid threading violations
        APP_LOG(AS_INFO) << "DirectCallerClient posting Start() to signaling thread";
        this->signaling_thread()->PostTask([this]() {
            APP_LOG(AS_INFO) << "DirectCallerClient calling Start() on signaling thread";
            this->Start();
        });
    });    
}

DirectCallerClient::~DirectCallerClient() { Disconnect(); }

bool DirectCallerClient::Initialize() {
    if (initialized_) {
        return true;
    }
    
    APP_LOG(AS_INFO) << "Initializing DirectCallerClient for user: " << opts_.user_name;
    // Call base class Initialize to set up WebRTC threads and members
    if (!DirectCaller::Initialize()) {
        APP_LOG(AS_ERROR) << "DirectCallerClient: base initialize failed";
        return false;
    }

    initialized_ = true;
    return true;
}

bool DirectCallerClient::Connect() {
    if (!initialized_) {
        APP_LOG(AS_ERROR) << "DirectCallerClient not initialized";
        return false;
    }

    // Set up callbacks for signaling server communication
    signaling_client_->setPeerJoinedCallback([this](const std::string& peer_id) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Peer joined signaling server: " << peer_id;
        this->onPeerJoined(peer_id);
    });

    // Receive ADDRESS:<user>:<ip>:<port> messages for any peer.
    // We only act on the one that matches opts_.target_name (if set).
    signaling_client_->setAddressReceivedCallback(
        [this](const std::string& user_id,
               const std::string& ip,
               int               port) {
            if (shutting_down_) {
                APP_LOG(AS_INFO) << "Caller shutting down – address ignored";
                return;
            }
            this->onPeerAddressResolved(user_id, ip, port);
        });

    std::string server_host; int server_port_int = 0;
    ParseIpAndPort(opts_.address, server_host, server_port_int);

    bool is_local = (server_host == "127.0.0.1" || server_host == "localhost");
    if (is_local && !opts_.target_name.empty()) {
        fprintf(stderr, "Caller: local address, direct TCP to %s:%d\n",
                server_host.c_str(), server_port_int);
        bool ok = DirectCaller::Connect(server_host.c_str(), server_port_int);
        fprintf(stderr, "Caller: Connect returned %s\n", ok ? "true" : "false");
        return ok;
    }

    if (!signaling_client_->connectToSignalingServer(server_host, std::to_string(server_port_int))) {
        APP_LOG(AS_WARNING) << "Failed to connect to signaling server, falling back to direct connection";
        
        if (!opts_.target_name.empty()) {
            APP_LOG(AS_INFO) << "DirectCallerClient: Attempting direct connection to " << opts_.target_name;
            std::string target_ip;
            int target_port = 0;
            ParseIpAndPort(opts_.address, target_ip, target_port);
            
            APP_LOG(AS_INFO) << "DirectCallerClient: Connecting directly to " << target_ip << ":" << target_port;
            initiateWebRTCCall(target_ip, target_port);
            return true;
        } else {
            APP_LOG(AS_ERROR) << "No signaling server and no target specified for direct connection";
            return false;
        }
    }

    // Register with room for name-based discovery
    signaling_client_->registerWithRoom(opts_.room_name);

    // Request initial user list
    signaling_client_->requestUserList();

    // Advertise our best local/public IPv4 so the signaling server contains
    // a realistic entry for this client. Although the callee normally
    // connects back to us only over WebRTC, some server implementations
    // insist on having a non-placeholder ADDRESS before they forward HELLO.

    // First try STUN to discover our public endpoint
    std::string pub_ip;
    uint16_t    pub_port = 0;
    bool        stun_ok  = false;
    int tmp_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (tmp_sock >= 0) {
        stun_ok = stun_discover(tmp_sock, "stun.l.google.com", 19302,
                                  pub_ip, pub_port);
        ::close(tmp_sock);
    }

    std::string ip_to_send;
    int         port_to_send = 0;

    if (stun_ok) {
        ip_to_send   = pub_ip;
        port_to_send = static_cast<int>(pub_port);
    } else {
        // Fallback to best local/non-loopback IP
        auto choose_best_ip = []() -> std::string {
            auto is_private_ip = [](const std::string& addr) {
                return addr.rfind("10.", 0) == 0 ||
                       addr.rfind("192.168.", 0) == 0 ||
                       std::regex_match(addr, std::regex(R"(^172\.(1[6-9]|2[0-9]|3[01])\.)"));
            };

            std::string private_ip;
            std::string first_non_loopback;

#if !defined(_WIN32)
            struct ifaddrs* ifaddr = nullptr;
            if (getifaddrs(&ifaddr) == 0) {
                for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                    if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP))
                        continue;
                    if (ifa->ifa_addr->sa_family != AF_INET)
                        continue;

                    char ip[INET_ADDRSTRLEN];
                    if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip)))
                        continue;

                    std::string candidate(ip);
                    if (candidate == "127.0.0.1")
                        continue;

                    if (first_non_loopback.empty())
                        first_non_loopback = candidate;

                    if (is_private_ip(candidate)) {
                        private_ip = candidate;
                        break;
                    }
                }
                freeifaddrs(ifaddr);
            }
#endif // !_WIN32

            if (!private_ip.empty()) return private_ip;
            if (!first_non_loopback.empty()) return first_non_loopback;
            return "127.0.0.1";
        };

        ip_to_send = choose_best_ip();
        port_to_send = 0; // no listening socket
    }

    signaling_client_->sendAddress(opts_.user_name, ip_to_send, port_to_send);

    // Immediately attempt to contact the target if one is specified. This is
    // important when using the lightweight raw-WebSocket signaling path which
    // currently does not broadcast explicit "peer-joined" events. By reusing
    // the existing onPeerJoined() handler we make sure that a targeted HELLO
    // is sent and that the direct WebRTC connection attempt (to 127.0.0.1:8888
    // by default) is kicked off without waiting indefinitely.
    if (!opts_.target_name.empty()) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Proactively initiating call to target " << opts_.target_name;
        onPeerJoined(opts_.target_name); // Re-use existing logic
    }

    // Note: onPeerJoined() above already takes care of sending the initial
    // HELLO towards the callee when we proactively trigger it.
    
    APP_LOG(AS_INFO) << "DirectCallerClient connected to signaling server for name resolution";
    return true;
}

void DirectCallerClient::Disconnect() {
    // Disconnect inherited DirectCaller resources
    DirectCaller::Disconnect();
    // Reset connection-related state to allow for a new call
    initialized_ = false;
    call_started_ = false;  // allow next ADDRESS to trigger a fresh dial
    APP_LOG(AS_INFO) << "DirectCallerClient disconnected and state reset for new call";
}

bool DirectCallerClient::IsConnected() const {
    return (this->peer_connection() != nullptr) || (signaling_client_ && signaling_client_->isConnected());
}

void DirectCallerClient::RunOnBackgroundThread() {
    // If we are currently on an rtc::Thread (i.e., WebRTC internal thread),
    // spawn a native thread to satisfy the DCHECK in DirectApplication.
    if (rtc::Thread::Current()) {
        std::thread([this]() {
            DirectCaller::RunOnBackgroundThread();
        }).detach();
    } else {
        DirectCaller::RunOnBackgroundThread();
    }
}

bool DirectCallerClient::WaitUntilConnectionClosed(int timeout_ms) {
    return DirectPeer::WaitUntilConnectionClosed(timeout_ms);
}

void DirectCallerClient::onPeerJoined(const std::string& peer_id) {
    // Map generic alias back to actual target when possible
    std::string resolved_peer_id = peer_id;
    if (peer_id == "callee" && !opts_.target_name.empty()) {
        resolved_peer_id = opts_.target_name;
    }

    APP_LOG(AS_INFO) << "DirectCallerClient: Peer joined: " << resolved_peer_id;
    
    // Don't call ourselves
    if (resolved_peer_id == opts_.user_name) {
        return;
    }
    
    // Only call the target user if specified
    if (!opts_.target_name.empty() && resolved_peer_id != opts_.target_name) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Ignoring peer " << resolved_peer_id << " (target is " << opts_.target_name << ")";
        return;
    }
    
    // Send HELLO now that the callee is present
    if (!opts_.target_name.empty()) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Target peer appeared – sending HELLO";
        signaling_client_->sendHelloToUser(opts_.target_name);
    }
    
    // Connection attempt will be made when we receive an ADDRESS message
    APP_LOG(AS_INFO) << "DirectCallerClient: Waiting for address resolution for " << resolved_peer_id;
}

void DirectCallerClient::onPeerAddressResolved(const std::string& peer_id,
                                               const std::string& ip,
                                               int port) {

    if (shutting_down_.load()) return;
    
    auto is_private_ip = [](const std::string& addr) {
        return addr.rfind("10.",   0)   == 0 ||
                addr.rfind("192.168.", 0) == 0 ||
                addr.rfind("127.", 0) == 0 ||
                std::regex_match(addr, std::regex(R"(^172\.(1[6-9]|2[0-9]|3[01])\.)"));
    };

    // Only act on the target user (if specified)
    if (!opts_.target_name.empty() && peer_id != opts_.target_name) {
        return;
    }

    // Log the current state to diagnose why a call might not start
    APP_LOG(AS_INFO) << "DirectCallerClient: Address resolved for " << peer_id
                     << " -> " << ip << ":" << port << ", call_started_: " << (call_started_.load() ? "true" : "false");

    if (call_started_.load()) {
        // Already trying one address – remember this one as a fallback.
        APP_LOG(AS_INFO) << "DirectCallerClient: Queuing additional address " << ip << ":" << port;
        pending_ip_   = ip;
        pending_port_ = port;
        return;
    }

    bool is_private = is_private_ip(ip);

    if (is_private) {
        // Prefer immediate connect for LAN addresses.
        call_started_ = true;
        APP_LOG(AS_INFO) << "DirectCallerClient: Detected private address – dialing immediately " << ip << ":" << port;
        bool connect_success = initiateWebRTCCall(ip, port);
        if (!connect_success) {
            APP_LOG(AS_WARNING) << "DirectCallerClient: Connection attempt failed, resetting state and checking for pending address";
            call_started_ = false;
            if (!pending_ip_.empty()) {
                std::string fallback_ip = pending_ip_;
                int fallback_port = pending_port_;
                pending_ip_.clear();
                pending_port_ = 0;
                APP_LOG(AS_INFO) << "DirectCallerClient: Falling back to queued address " << fallback_ip << ":" << fallback_port;
                onPeerAddressResolved(peer_id, fallback_ip, fallback_port);  // Recurse to try fallback
            }
        }
        return;
    }

    // First address is public – remember it and wait briefly for a possible private one.
    APP_LOG(AS_INFO) << "DirectCallerClient: Received public address first, will wait 400 ms for LAN alternative.";
    pending_ip_   = ip;
    pending_port_ = port;

    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        if (!call_started_.load() && !pending_ip_.empty()) {
            call_started_ = true;
            std::string ip_to_dial  = pending_ip_;
            int         port_to_dial = pending_port_;
            pending_ip_.clear();
            pending_port_ = 0;
            APP_LOG(AS_INFO) << "DirectCallerClient: Fallback to public address after wait " << ip_to_dial << ":" << port_to_dial;
            bool connect_success = initiateWebRTCCall(ip_to_dial, port_to_dial);
            if (!connect_success) {
                APP_LOG(AS_WARNING) << "DirectCallerClient: Fallback connection attempt failed, resetting state";
                call_started_ = false;
            }
        }
    }).detach();
}

// initiateWebRTCCall definition moved to DirectPeer base class.

void DirectCallerClient::onAnswerReceived(const std::string& peer_id, const std::string& sdp) {
    // This would be handled by DirectCaller's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCallerClient: Answer received from " << peer_id;
}

void DirectCallerClient::onIceCandidateReceived(const std::string& peer_id, const std::string& candidate) {
    // This would be handled by DirectCaller's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCallerClient: ICE candidate received from " << peer_id;
}

bool DirectCallerClient::RequestUserList() {
    if (signaling_client_) {
        return signaling_client_->requestUserList();
    }
    return false;
}

// DirectCalleeClient Implementation  

DirectCalleeClient::DirectCalleeClient(const Options& opts)
    : DirectCallee(opts), initialized_(false), listening_(false) {
    fprintf(stderr, "ctor DirectCalleeClient begin\n");
    active_peer_id_ = "";
    // Parse the intended listening port from opts_.address instead of forcing 0
    std::string temp_ip;
    ParseIpAndPort(opts_.address, temp_ip, local_port_);
    fprintf(stderr, "ctor DirectCalleeClient before DirectClient\n");
    signaling_client_ = std::make_unique<DirectClient>(opts);
    fprintf(stderr, "ctor DirectCalleeClient after DirectClient\n");
    // Provide busy predicate: callee is busy while a PeerConnection exists
    signaling_client_->setIsBusyCallback([this]() {
        // Only consider busy if there's an active call with remote description set
        bool has_pc = this->peer_connection() != nullptr;
        bool has_remote_desc = has_pc && this->peer_connection()->remote_description() != nullptr;
        bool active_call = has_pc && has_remote_desc;
        APP_LOG(AS_INFO) << "Busy check: has_pc=" << (has_pc ? "true" : "false") 
                        << ", has_remote_desc=" << (has_remote_desc ? "true" : "false")
                        << ", returning=" << (active_call ? "BUSY" : "AVAILABLE");
        return active_call;        
    });
    fprintf(stderr, "ctor DirectCalleeClient end\n");
}

DirectCalleeClient::~DirectCalleeClient() {
    StopListening();
}

bool DirectCalleeClient::Initialize() {
    if (initialized_) {
        return true;
    }
    
    APP_LOG(AS_INFO) << "Initializing DirectCalleeClient for user: " << opts_.user_name;
    initialized_ = true;
    return true;
}

bool DirectCalleeClient::StartListening() {
    // Disallow duplicate starts while already listening
    if (listening_) {
        APP_LOG(AS_INFO) << "DirectCalleeClient is already listening – ignoring repeat call.";
        return true;
    }

    if (!initialized_) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient not initialized";
        return false;
    }

    // Prepare for a fresh run after a previous shutdown.
    should_quit_.store(false);
    cleaned_up_.store(false);
    ResetConnectionClosedEvent();

    // Preserve original signaling address before we overwrite opts_.address for the local listener
    std::string signaling_address = opts_.address;  // e.g. "127.0.0.1:3456"

    // First, set up WebRTC listener using DirectCallee (this will update opts_.address)
    setupWebRTCListener();
    
    // Set up callbacks to receive incoming calls via signaling server
    signaling_client_->setHelloReceivedCallback([this](const std::string& peer_id) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Received HELLO from " << peer_id;
        this->onIncomingCall(peer_id, "");
    });
    
    signaling_client_->setOfferReceivedCallback([this](const std::string& peer_id, const std::string& sdp) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Received offer from " << peer_id;
        this->onIncomingCall(peer_id, sdp);
    });

    signaling_client_->setIceCandidateReceivedCallback([this](const std::string& peer_id, const std::string& candidate_json) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: ICE candidate received from " << peer_id;

        // Only act on our current peer, ignore others.
        if (!opts_.target_name.empty() && peer_id != opts_.target_name) {
            return;
        }

        std::string cand_str = candidate_json;
        int mline_index = 0;

        if (!candidate_json.empty() && candidate_json.front() == '{') {
            Json::CharReaderBuilder rb;
            std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
            Json::Value root;
            std::string errs;
            if (reader->parse(candidate_json.c_str(), candidate_json.c_str() + candidate_json.size(), &root, &errs)) {
                if (root.isMember("candidate")) {
                    cand_str = root["candidate"].asString();
                }
                if (root.isMember("sdpMLineIndex")) {
                    mline_index = root["sdpMLineIndex"].asInt();
                }
            }
        }

        this->AddIceCandidate(cand_str, mline_index);
    });

    // Forward raw INVITE lines (with JSON payload) so DirectPeer can extract agent info.
    signaling_client_->setInviteReceivedCallback([this](const std::string& raw_message) {
        // Pass through to DirectPeer message handler; no socket context in WSS path.
        this->HandleMessage(nullptr, raw_message, rtc::SocketAddress());
    });
    
    // Forward raw WAITING lines (with JSON payload) so DirectPeer can extract agent info.
    signaling_client_->setWaitingReceivedCallback([this](const std::string& raw_message) {
        // Pass through to DirectPeer message handler; no socket context in WSS path.
        this->HandleMessage(nullptr, raw_message, rtc::SocketAddress());
    });

    // Try to connect to signaling server to register our presence
    // But don't fail if signaling server is unavailable
    std::string server_host; int server_port_int = 0;
    ParseIpAndPort(signaling_address, server_host, server_port_int);

    // Skip signaling server if it points to our own listener (would self-connect)
    bool is_self = (server_host == "127.0.0.1" || server_host == "localhost" || server_host == "0.0.0.0")
                   && server_port_int == local_port_;
    if (is_self) {
        fprintf(stderr, "Callee: signaling address matches own listener, skipping self-connect\n");
    }

    auto try_register = [this, server_host, server_port_int, is_self]() {
        if (is_self) return;

        const int kMaxAttempts = 30;
        int attempt = 0;
        while (!this->should_quit_.load() && attempt < kMaxAttempts) {
            if (signaling_client_->connectToSignalingServer(server_host, std::to_string(server_port_int))) {
                APP_LOG(AS_INFO) << "Connected to signaling server on attempt " << (attempt + 1);
                signaling_client_->registerWithRoom(opts_.room_name);
                publishAddressToSignalingServer();
                APP_LOG(AS_INFO) << "DirectCalleeClient registered with signaling server";
                return;
            }
            ++attempt;
            APP_LOG(AS_WARNING) << "Attempt " << attempt << " to connect to signaling server failed – retrying in 1 s";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!this->should_quit_.load()) {
            APP_LOG(AS_ERROR) << "Unable to connect to signaling server after " << kMaxAttempts << " attempts; callee will still accept direct connections on port " << local_port_;
        }
    };

    // Run the registration attempts on a detached std::thread so we don't block StartListening().
    std::thread(try_register).detach();
    
    listening_ = true;
    APP_LOG(AS_INFO) << "DirectCalleeClient listening for calls in room: " << opts_.room_name;
    APP_LOG(AS_INFO) << "DirectCalleeClient: WebRTC calls can connect directly to port " << local_port_;
    return true;
}

void DirectCalleeClient::StopListening() {
    // Guard against repeated calls and break potential recursion with SignalQuit().
    if (!listening_) {
        return;  // Already stopped.
    }

    listening_ = false;  // Mark as no longer listening *before* further cleanup.

    // Close active TCP sockets so the port becomes immediately reusable.
    if (tcp_socket_) {
        tcp_socket_->Close();
        tcp_socket_.reset();
    }

    // Close the listening socket (if still open) to free the port.
    if (listen_socket_) {
        listen_socket_.reset();  // Destructor closes the underlying OS socket
    }

    // Pause WebSocket listener but keep TCP connection alive so registration persists.
    if (signaling_client_) {
        signaling_client_->pause();
    }

    shutting_down_.store(true);
    
    // Disconnect WebRTC and tear down per-connection state without a full cleanup.
    DirectApplication::Disconnect();

    // Finally, perform the generic quit signalling – this will wake up any waiting loops.
    DirectPeer::SignalQuit();

    APP_LOG(AS_INFO) << "DirectCalleeClient stopped listening";
}

bool DirectCalleeClient::IsListening() const {
    return listening_;
}

bool DirectCalleeClient::IsConnected() const {
    return (this->peer_connection() != nullptr) || (signaling_client_ && signaling_client_->isConnected());
}

void DirectCalleeClient::RunOnBackgroundThread() {
    DirectCallee::RunOnBackgroundThread();
    APP_LOG(AS_INFO) << "DirectCalleeClient running on background thread";
}

bool DirectCalleeClient::WaitUntilConnectionClosed(int timeout_ms) {
    return DirectPeer::WaitUntilConnectionClosed(timeout_ms);
}

void DirectCalleeClient::ResetConnectionClosedEvent() {
    DirectPeer::ResetConnectionClosedEvent();
    APP_LOG(AS_INFO) << "DirectCalleeClient: Connection closed event reset";
}

void DirectCalleeClient::SignalQuit() {
    APP_LOG(AS_INFO) << "DirectCalleeClient: synchronous quit starting";
    shutting_down_.store(true);
    should_quit_.store(true);
    listening_ = false;

    auto close_tcp = [this]() {
        if (tcp_socket_) {
            auto* s = tcp_socket_.release();
            s->Close();
            delete s;
        }
    };
    if (network_thread() && !network_thread()->IsCurrent()) {
        network_thread()->BlockingCall(close_tcp);
    } else {
        close_tcp();
    }

    auto close_listener = [this]() {
        if (listen_socket_) {
            listen_socket_.reset();
        }
    };
    if (network_thread() && !network_thread()->IsCurrent()) {
        network_thread()->BlockingCall(close_listener);
    } else {
        close_listener();
    }

    DirectApplication::Disconnect();
    connection_closed_event_.Set();
    APP_LOG(AS_INFO) << "DirectCalleeClient: synchronous quit complete";
}

// -------------------------------------------------------------------
//  Override SendMessage so generic strings (LLAMA:, WHISPER:, etc.) are
//  forwarded through the WebSocket signalling path when available.
// -------------------------------------------------------------------
bool DirectCalleeClient::SendMessage(const std::string& message) {
    // Use signaling server if connected; fall back to base implementation.
    // Status code responses (200 OK, 486 Busy Here, etc.) should always go via TCP
    // to respond to the connection that sent the original message (HELLO, BYE, etc.)
    if (StatusCodes::IsStatusCode(message)) {
        // Send status codes via TCP connection (base implementation)
        return DirectCallee::SendMessage(message);
    }

    if (signaling_client_ && signaling_client_->isConnected()) {
        signaling_client_->ws_client()->send_message(message);
        return true;
    }

    // Fall back to base implementation if signaling server not available
    return DirectCallee::SendMessage(message);
}

void DirectCalleeClient::setupWebRTCListener() {
    APP_LOG(AS_INFO) << "DirectCalleeClient: Setting up WebRTC listener on port " << local_port_;
    
    // Ensure opts_ has the correct listen address to avoid conflict with signaling port
    opts_.address = "0.0.0.0:" + std::to_string(local_port_);

    if (!DirectCallee::Initialize()) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient: base initialization failed";
        return;
    }

    if (!DirectCallee::StartListening()) {
        APP_LOG(AS_ERROR) << "Failed to start WebRTC listener on port " << local_port_;
        return;
    }

    // Ensure a local video source is ready so the SDP answer advertises sendrecv.
    if (opts_.video) {
        if (!video_source_) {
            // When using a talking-face avatar, defer video source creation
            // until the offer/answer path so the PeerConnection attaches the
            // avatar source instead of seeding an Echo source here.
            if (!opts_.talking_face.empty()) {
                APP_LOG(AS_INFO) << "Callee will create talking-face source during answer setup";
            } else if (!opts_.camera.empty()) {
                
                if (rtc::Thread::Current() == signaling_thread()) {
                    video_source_ = CreateCameraVideoSource(this, opts_);
                } else {
                    signaling_thread()->BlockingCall([this]() {
                        video_source_ = CreateCameraVideoSource(this, opts_);
                    });
                }                
            }
            if (!video_source_ && opts_.talking_face.empty()) {
                APP_LOG(AS_INFO) << "Callee fallback to synthetic video source";
                if (rtc::Thread::Current() == signaling_thread()) {
                    auto src = rtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                    video_source_ = src;
                } else {
                    signaling_thread()->BlockingCall([this]() {
                        auto src = rtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                        video_source_ = src;
                    });
                }
            }
            SetVideoSource(video_source_);
        }
    }

    // Resume WebSocket listener now that we are ready to accept new calls.
    if (signaling_client_) {
        signaling_client_->resume();
    }

    APP_LOG(AS_INFO) << "DirectCalleeClient: WebRTC listener started on port " << local_port_;
}

void DirectCalleeClient::publishAddressToSignalingServer() {
    if (!signaling_client_ || !signaling_client_->isConnected()) {
        APP_LOG(AS_WARNING) << "Cannot publish address – signaling client not connected";
        return;
    }

    // Discover the best IPv4 address to advertise.
    // Prefer RFC1918 private addresses (10/172.16-31/192.168) when available
    // so that peers on the same LAN can connect directly.  Fall back to the
    // first non-loopback address, and ultimately to 127.0.0.1 when running
    // both caller and callee on the same host.

    auto is_private_ip = [](const std::string& addr) {
        return addr.rfind("10.", 0) == 0 ||
               addr.rfind("192.168.", 0) == 0 ||
               std::regex_match(addr, std::regex(R"(^172\.(1[6-9]|2[0-9]|3[01])\.)"));
    };

    std::string lan_ip = "127.0.0.1";           // default
    std::string first_non_loopback;

#if !defined(_WIN32)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP))
                continue;

            if (ifa->ifa_addr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) {
                    std::string candidate(ip);
                    if (candidate == "127.0.0.1")
                        continue; // loopback – keep as last resort

                    if (is_private_ip(candidate)) {
                        lan_ip = candidate;  // best choice
                        break;
                    }

                    if (first_non_loopback.empty()) {
                        first_non_loopback = candidate; // remember for fallback
                    }
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif // _WIN32

    // If no private IP found, use the first non-loopback one we saw.
    if (lan_ip == "127.0.0.1" && !first_non_loopback.empty()) {
        lan_ip = first_non_loopback;
    }

    // Advertise only the LAN address that corresponds to the TCP listener.
    // Publishing the STUN‐derived (UDP) endpoint resulted in the caller
    // attempting to establish a TCP connection on a port that is only open
    // for UDP, yielding "connection refused" errors.  If you later add a
    // TCP‐capable NAT traversal mechanism (e.g. RFC 6062, TURN over TCP),
    // you can re-enable public address advertisement here.

    signaling_client_->sendAddress(opts_.user_name, lan_ip, local_port_);
}

void DirectCalleeClient::onIncomingCall(const std::string& peer_id, const std::string& sdp) {
    APP_LOG(AS_INFO) << "DirectCalleeClient: Incoming call from peer: " << peer_id;
    active_peer_id_ = peer_id; // remember for ICE candidate routing

    if (shutting_down_.load()) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Shutting down, ignoring incoming call from " << peer_id;
        return;
    }

    // If a PeerConnection already exists, determine whether it is actually in use.
    // Accept the OFFER as long as we have not yet set a remote description (i.e. first
    // call leg) – this happens because StartListening() pre-creates the PC.
    if (peer_connection()) {
        bool already_in_call = peer_connection()->remote_description() != nullptr;
        if (already_in_call) {
            APP_LOG(AS_WARNING) << "DirectCalleeClient: Already in an active call, rejecting new call from " << peer_id;
            if (signaling_client_) {
                signaling_client_->sendCancel();
            }
            return;
        }
    }

    // Ensure WebRTC layer is initialized and listening.
    if (!initialized_) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient: Not initialized, cannot accept call";
        return;
    }

    if (!listening_) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Not listening, starting WebRTC listener";
        setupWebRTCListener();
        listening_ = true;
    }

    // We must have a valid PeerConnection to handle the offer.
    if (!peer_connection()) {
        if (!CreatePeerConnection()) {
            APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to create PeerConnection";
            return;
        }
    }

    // Process the incoming SDP offer.
    if (!sdp.empty()) {
        std::string raw_sdp = sdp;
        if (!sdp.empty() && sdp.front() == '{') {
            // Attempt to parse JSON‐wrapped SessionDescription.
            Json::CharReaderBuilder rbuilder;
            std::unique_ptr<Json::CharReader> reader(rbuilder.newCharReader());
            Json::Value root;
            std::string errs;
            if (reader->parse(sdp.c_str(), sdp.c_str() + sdp.size(), &root, &errs)) {
                if (root.isMember("sdp")) {
                    if (root["sdp"].isString()) {
                        raw_sdp = root["sdp"].asString();
                    } else if (root["sdp"].isObject() && root["sdp"].isMember("sdp")) {
                        raw_sdp = root["sdp"]["sdp"].asString();
                    }
                }
            }
        }

        webrtc::SdpParseError err;
        std::unique_ptr<webrtc::SessionDescriptionInterface> offer_desc =
            webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, raw_sdp, &err);
        if (!offer_desc) {
            APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to parse offer SDP from " << peer_id << ": " << err.description;
            return;
        }

        // Set the remote description asynchronously, then generate the answer.
        set_remote_description_observer_ = rtc::make_ref_counted<LambdaSetRemoteDescriptionObserver>(
            [this, peer_id](webrtc::RTCError error) {
                if (!error.ok()) {
                    APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to set remote description: " << error.message();
                    return;
                }
                APP_LOG(AS_INFO) << "DirectCalleeClient: Remote description set – creating answer";
                this->createAndSendAnswer(peer_id);
            });

        peer_connection()->SetRemoteDescription(std::move(offer_desc), set_remote_description_observer_);
    } else {
        APP_LOG(AS_INFO) << "DirectCalleeClient: No SDP provided with HELLO; waiting for OFFER";
    }
}

// Helper that creates an SDP answer and sends it back via the signaling server
void DirectCalleeClient::createAndSendAnswer(const std::string& peer_id) {
    if (!peer_connection()) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient: Cannot create answer – peer connection is null";
        return;
    }

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions answer_opts;

    create_session_observer_ = rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>(
        [this, peer_id](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
            std::string local_sdp;
            desc->ToString(&local_sdp);

            #if 0
            // Force DTLS role to passive to avoid both sides negotiating as active (client)
            const std::string kSetupActive = "a=setup:active";
            const std::string kSetupPassive = "a=setup:passive";
            size_t setup_pos = 0;
            bool patched = false;
            while ((setup_pos = local_sdp.find(kSetupActive, setup_pos)) != std::string::npos) {
                local_sdp.replace(setup_pos, kSetupActive.size(), kSetupPassive);
                setup_pos += kSetupPassive.size();
                patched = true;
            }
            if (patched) {
                APP_LOG(AS_INFO) << "DirectCalleeClient: Patched all setup:active lines to passive in SDP answer.";
                APP_LOG(AS_INFO) << "DirectCalleeClient: SDP answer: " << local_sdp;
            } else {
                APP_LOG(AS_WARNING) << "DirectCalleeClient: No setup:active found to patch – DTLS role may already be passive.";
            }
            #endif
            // Create a new SessionDescription from the potentially modified SDP so that
            // the PeerConnection uses the same (passive) DTLS role we will send to the browser.
            webrtc::SdpParseError parse_err;
            std::unique_ptr<webrtc::SessionDescriptionInterface> patched_desc =
                webrtc::CreateSessionDescription(desc->GetType(), local_sdp, &parse_err);
            if (!patched_desc) {
                APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to create patched SessionDescription: " << parse_err.description;
                return;
            }

            set_local_description_observer_ = rtc::make_ref_counted<LambdaSetLocalDescriptionObserver>(
                [this, peer_id, local_sdp](webrtc::RTCError error) {
                    if (!error.ok()) {
                        APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to set local description: " << error.message();
                        return;
                    }
                    APP_LOG(AS_INFO) << "DirectCalleeClient: Local description set – sending ANSWER to " << peer_id;

                    // Wrap SDP into JSON similar to incoming offer for browser compatibility.
                    Json::Value root;
                    Json::Value sdpObj;
                    sdpObj["type"] = "answer";
                    sdpObj["sdp"]  = local_sdp;
                    root["sdp"] = sdpObj;
                    Json::StreamWriterBuilder wbuilder;
                    wbuilder["indentation"] = ""; // no pretty print
                    std::string json_answer = Json::writeString(wbuilder, root);

                    if (signaling_client_) {
                        signaling_client_->sendAnswer(peer_id, json_answer);
                    } else {
                        APP_LOG(AS_ERROR) << "DirectCalleeClient: signaling_client_ is null – cannot send ANSWER";
                    }
                });

            peer_connection()->SetLocalDescription(std::move(patched_desc), set_local_description_observer_);
        });

    peer_connection()->CreateAnswer(create_session_observer_.get(), answer_opts);
}

void DirectCalleeClient::onIceCandidateReceived(const std::string& peer_id, const std::string& candidate) {
    // This would be handled by DirectCallee's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCalleeClient: ICE candidate received from " << peer_id;
}

// -----------------------------------------------------------------------------
// DirectCalleeClient ICE handling

void DirectCalleeClient::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    // Convert candidate to string
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient: Failed to serialize ICE candidate";
        return;
    }

    int mline_index = candidate->sdp_mline_index();

    // Build JSON payload compatible with browser side
    Json::Value root;
    root["candidate"] = sdp;
    root["sdpMid"] = candidate->sdp_mid();
    root["sdpMLineIndex"] = mline_index;
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    std::string json_candidate = Json::writeString(wbuilder, root);

    APP_LOG(AS_INFO) << "DirectCalleeClient: Sending ICE candidate: " << json_candidate;

    const std::string target_id = !active_peer_id_.empty() ? active_peer_id_ : opts_.target_name;
    if (signaling_client_ && signaling_client_->isConnected()) {
        signaling_client_->sendIceCandidate(target_id, json_candidate);
    } else {
        // Fallback: send as raw text string over the local TCP socket to signal_bridge
        std::string raw_ice = "ICE:" + std::to_string(mline_index) + ":" + sdp;
        SendMessage(raw_ice);
    }

    // Optionally still call base to keep internal bookkeeping but suppress raw socket send
    // We skip DirectPeer::OnIceCandidate to avoid SendMessage use unless fallback triggers.
}

void DirectCalleeClient::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    DirectCallee::OnIceConnectionChange(new_state);
    std::string state_name;
    switch (new_state) {
        case webrtc::PeerConnectionInterface::kIceConnectionNew: state_name = "New"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionChecking: state_name = "Checking"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionConnected: state_name = "Connected"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionCompleted: state_name = "Completed"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionFailed: state_name = "Failed"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: state_name = "Disconnected"; break;
        case webrtc::PeerConnectionInterface::kIceConnectionClosed: state_name = "Closed"; break;
        default: state_name = "Unknown(" + std::to_string(static_cast<int>(new_state)) + ")"; break;
    }
    APP_LOG(AS_INFO) << "DirectCalleeClient: ICE connection state changed to: " << state_name;
    if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed ||
        new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected) {
        APP_LOG(AS_WARNING) << "DirectCalleeClient: ICE connection failed or disconnected, media may not flow.";
    } else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
               new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: ICE connection established, media should flow if DTLS is complete.";
    }
}

void DirectCalleeClient::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    DirectCallee::OnConnectionChange(new_state);
    std::string state_name;
    switch (new_state) {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kNew: state_name = "New"; break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting: state_name = "Connecting"; break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected: state_name = "Connected"; break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected: state_name = "Disconnected"; break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed: state_name = "Failed"; break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed: state_name = "Closed"; break;
        default: state_name = "Unknown(" + std::to_string(static_cast<int>(new_state)) + ")"; break;
    }
    APP_LOG(AS_INFO) << "DirectCalleeClient: Peer connection state changed to: " << state_name;
    if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Peer connection fully established, media should be flowing.";
    } else if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
               new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected) {
        APP_LOG(AS_WARNING) << "DirectCalleeClient: Peer connection failed or disconnected, media will not flow.";
    }
}

// Removed OnDtlsStateChange due to missing enum values in this WebRTC version
// -----------------------------------------------------------------------------
// DirectClient Implementation (restored)

DirectClient::DirectClient(const Options& opts)
    : opts_(opts), connected_(false), registered_(false) {
    invite_received_callback_ = nullptr;
    waiting_received_callback_ = nullptr;
    // Create a dedicated network thread for WebSocket operations first
    network_thread_ = rtc::Thread::CreateWithSocketServer();
    network_thread_->SetName("WebSocketNetworkThread", nullptr);
    network_thread_->Start();

    // Now create the WebSocket client and attach the thread.  Must use
    // std::make_shared because WebSocketClient derives from
    // enable_shared_from_this and calls shared_from_this() internally.
    ws_client_ = std::make_shared<WebSocketClient>();
    ws_client_->set_network_thread(network_thread_.get());

    default_ws_handler_ = [this](const std::string& message) {
        this->handleProtocolMessage(message);
    };
    ws_client_->set_message_callback(default_ws_handler_);

    ws_client_->set_reconnect_callback([this]() {
        // Re-register and re-publish address when socket comes back
        if (!saved_room_.empty()) {
            ws_client_->send_message("REGISTER:" + opts_.user_name + ":" + saved_room_);
        }
        if (!pending_address_.empty()) {
            ws_client_->send_message(pending_address_);
        }
    });

    // Keep WebSocket connected; do not disconnect here.
}

DirectClient::~DirectClient() {
    if (network_thread_) {
        auto* nt = network_thread_.get();

        if (rtc::Thread::Current() == nt) {
            // Already executing on the network thread – cannot use BlockingCall
            // and cannot join this thread from itself.  Do immediate cleanup
            // then hand-off the join to a detached std::thread.

            if (ws_client_) {
                ws_client_->set_allow_reconnect(false);
                ws_client_->disconnect();
            }

            // Move ownership of ws_client_ and network_thread_ to the joiner
            auto ws = std::move(ws_client_);
            auto nt_owned = network_thread_.release();

            std::thread([ws = std::move(ws), nt_owned]() mutable {
                // ws first to ensure no tasks access thread after Stop()
                ws.reset();
                if (nt_owned) {
                    nt_owned->Stop();
                    delete nt_owned;
                }
            }).detach();

        } else {
            // Normal case – we are on a different thread and can block.
            if (ws_client_) {
                rtc::Event done;
                nt->PostTask([this, &done]() {
                    ws_client_->set_allow_reconnect(false);
                    ws_client_->disconnect();
                    done.Set();
                });
                // Wait up to 5s for the disconnect task to execute.
                done.Wait(webrtc::TimeDelta::Seconds(5));
            }

            // Allow remaining tasks through the normal Stop() flush.
            nt->Stop();
            network_thread_.reset();

            ws_client_.reset();
        }
    } else {
        // No network thread – just delete client if still present.
        ws_client_.reset();
    }
}

bool DirectClient::connectToSignalingServer(const std::string& server_host,
                                           const std::string& server_port) {
    if (connected_) return true;

    WebSocketClient::Config cfg;
    cfg.host = server_host;
    cfg.port = std::to_string(std::stoi(server_port) + 1);
    cfg.use_ssl = true;
    cfg.cert_file_path = opts_.webrtc_cert_path;
    cfg.key_file_path = opts_.webrtc_key_path;

    if (!ws_client_->connect(cfg)) {
        APP_LOG(AS_ERROR) << "Failed to connect to WebSocket server";
        return false;
    }

    if (!ws_client_->send_websocket_handshake_with_headers()) {
        APP_LOG(AS_ERROR) << "WebSocket HTTP handshake failed";
        return false;
    }

    ws_client_->start_listening();
    connected_ = true;
    APP_LOG(AS_INFO) << "Connected to signaling server: " << cfg.host << ":" << cfg.port;
    return true;
}

void DirectClient::registerWithRoom(const std::string& room_name) {
    if (!connected_) return;
    saved_room_ = room_name;
    std::string msg = "REGISTER:" + opts_.user_name + ":" + room_name;
    ws_client_->send_message(msg);
    registered_ = true;
}

bool DirectClient::isConnected() const { return connected_; }

// Simple wrappers still required
bool DirectClient::sendHelloToUser(const std::string& target_user_id) {
    if (!connected_) return false;
    return ws_client_->send_message(std::string(Msg::kHelloPrefix) + target_user_id);
}

bool DirectClient::requestUserList() {
    if (!connected_) return false;
    return ws_client_->send_message("USERS");
}

bool DirectClient::sendOffer(const std::string& target_peer_id, const std::string& sdp) {
    if (!connected_) return false;
    std::string msg = std::string(Msg::kOfferPrefix) + opts_.user_name + ":" + sdp;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendAnswer(const std::string& target_peer_id, const std::string& sdp) {
    if (!connected_) return false;
    std::string msg = std::string(Msg::kAnswerPrefix) + opts_.user_name + ":" + sdp;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendIceCandidate(const std::string& target_peer_id, const std::string& candidate) {
    if (!connected_) return false;
    std::string msg = "ICE:" + target_peer_id + ":" + candidate;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendInit() {
    if (!connected_) return false;
    return ws_client_->send_message(Msg::kInvite);
}

bool DirectClient::sendBye() {
    if (!connected_) return false;
    return ws_client_->send_message("BYE");
}

bool DirectClient::sendCancel() {
    if (!connected_) return false;
    return ws_client_->send_message(Msg::kCancel);
}

bool DirectClient::sendAddress(const std::string& user_id, const std::string& ip, int port) {
    if (!connected_) return false;
    std::string msg = "ADDRESS:" + user_id + ":" + ip + ":" + std::to_string(port);
    return ws_client_->send_message(msg);
}

// Stub methods (full parsing logic preserved elsewhere)
void DirectClient::disconnect() {
    if (ws_client_) ws_client_->disconnect();
    connected_ = false;
    registered_ = false;
}

// Minimal versions of other helpers to satisfy linker. Full behaviour kept earlier.
bool DirectClient::startTcpServer(int) { return false; }

void DirectClient::handleProtocolMessage(const std::string& message) {
    if (shutting_down_.load()) return;

    APP_LOG(AS_INFO) << "DirectClient received message: " << message;

    if (message.rfind(Msg::kHelloPrefix, 0) == 0 || message.rfind(Msg::kHello, 0) == 0) {
        // Targeted HELLO:HELLO:<caller id>
        std::string target = message.substr(6);
        bool busy = is_busy_callback_ ? is_busy_callback_() : false;
        const char* response = busy ? StatusCodes::kBusyHere : StatusCodes::kOk;
        APP_LOG(AS_INFO) << "HELLO is for us, sending " << response;
        ws_client_->send_message(response);

    } else if (message.rfind(Msg::kInvitePrefix, 0) == 0) {
        // Do the quick ACK so the browser proceeds
        APP_LOG(AS_INFO) << "DirectClient sending " << Msg::kWaiting;
        ws_client_->send_message(Msg::kWaiting);

        // Forward raw INVITE line to upper layer via callback.
        if (invite_received_callback_) {
            invite_received_callback_(message);
        }

        return; // stop further processing for INVITE – handled above
    }
    else if (message == Msg::kWaiting) {
        // Forward WAITING message to caller via dedicated callback
        if (waiting_received_callback_) {
            waiting_received_callback_(message);
        }
        return; // stop further processing for WAITING – handled above
    }
    
    else if (message.rfind(Msg::kAddressPrefix, 0) == 0) {
        // Format: ADDRESS:user_id:ip:port
        std::vector<std::string> parts;
        std::stringstream ss(message);
        std::string token;
        while (std::getline(ss, token, ':')) {
            parts.push_back(token);
        }
        if (parts.size() == 4) {
            std::string userId = parts[1];
            std::string ip = parts[2];
            int port = std::stoi(parts[3]);
            APP_LOG(AS_VERBOSE) << "DirectClient received ADDRESS for " << userId << " -> " << ip << ":" << port;
            if (address_received_callback_) {
                address_received_callback_(userId, ip, port);
            }
        } else {
            APP_LOG(AS_WARNING) << "ADDRESS message malformed: " << message;
        }

    } else if (message.rfind(Msg::kUsersPrefix, 0) == 0) {
        // Format accepted:
        //   "USERS:user1,user2"  (preferred)
        //   "USERS user1,user2"  (legacy – without colon)
        size_t delimiter = message.find_first_of(": ");
        std::string user_list_str;
        if (delimiter != std::string::npos) {
            user_list_str = message.substr(delimiter + 1);
        } else {
            // No delimiter – nothing to parse
            user_list_str = "";
        }
        std::vector<std::string> users;
        std::stringstream ss(user_list_str);
        std::string user;
        while (std::getline(ss, user, ',')) {
            if (!user.empty()) {
                users.push_back(user);
            }
        }
        APP_LOG(AS_INFO) << "DirectClient received USERS with " << users.size() << " users";
        if (user_list_received_callback_) {
            user_list_received_callback_(users);
        }

    } else if (message.rfind(Msg::kOfferPrefix, 0) == 0) {
        // Format: OFFER:peer_id:sdp
        size_t first_colon = message.find(':'); // after OFFER
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string sdp = message.substr(second_colon + 1);
            if (peerId == opts_.user_name) {
                return; // Ignore our own offer broadcast
            }
            APP_LOG(AS_INFO) << "DirectClient received OFFER from " << peerId;
            if (offer_received_callback_) {
                offer_received_callback_(peerId, sdp);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed OFFER message: " << message;
        }

    } else if (message.rfind(Msg::kAnswerPrefix, 0) == 0) {
        // Format: ANSWER:peer_id:sdp
        size_t first_colon = message.find(':');
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string sdp = message.substr(second_colon + 1);
            if (peerId == opts_.user_name) {
                return; // Ignore our own answer broadcast
            }
            APP_LOG(AS_INFO) << "DirectClient received ANSWER from " << peerId;
            if (answer_received_callback_) {
                answer_received_callback_(peerId, sdp);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed ANSWER message: " << message;
        }

    } else if (message.rfind(Msg::kIcePrefix, 0) == 0) {
        // Format: ICE:peer_id:candidate
        size_t first_colon = message.find(':');
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string candidate = message.substr(second_colon + 1);
            if (peerId == opts_.user_name) {
                return; // Ignore our own ICE broadcast
            }
            if (ice_candidate_received_callback_) {
                ice_candidate_received_callback_(peerId, candidate);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed ICE message: " << message;
        }
    }
    else if (message.front() == '{' && message.find("\"candidate\"") != std::string::npos) {
        if (ice_candidate_received_callback_) {
            ice_candidate_received_callback_("unknown_peer", message);
        }
    }
}

void DirectClient::pause() {
    if (ws_client_) {
        ws_client_->set_message_callback(nullptr); // keep connection alive
    }
}

void DirectClient::resume() {
    if (ws_client_) {
        ws_client_->set_message_callback(default_ws_handler_);
    }
}

// -----------------------------------------------------------------------------
// C-compatible wrapper implementation

struct _DirectUserListThunk {
    DirectUserListCallbackC  c_callback;
    void*                    c_context;
    // We keep the converted std::function alive by storing it in the struct so
    // that the lambda's captured std::string pointers stay valid for the
    // lifetime of the thunk (which we tie to the DirectCallerClient instance).
    std::function<void(const std::vector<std::string>&)>  cpp_func;
};

void DirectCallerClient_SetUserListCallbackC(DirectCallerClient*       client,
                                             DirectUserListCallbackC   callback,
                                             void*                     context) {
    if (!client || !callback) {
        return;
    }

    auto thunk = std::make_shared<_DirectUserListThunk>();
    thunk->c_callback = callback;
    thunk->c_context  = context;

    // Wrap C callback into std::function that DirectCallerClient expects.
    thunk->cpp_func = [thunk](const std::vector<std::string>& users) {
        // Convert std::vector<std::string> to array of const char* expected by C callback
        std::vector<const char*> c_strings;
        c_strings.reserve(users.size());
        for (const auto& s : users) {
            c_strings.push_back(s.c_str());
        }
        thunk->c_callback(c_strings.data(), static_cast<int>(c_strings.size()), thunk->c_context);
    };

    client->SetUserListCallback(thunk->cpp_func);

    // NOTE: We intentionally do *not* free the thunk; it will live as long as
    // the DirectCallerClient holds a copy of the std::function (which captures
    // the shared_ptr). When the client resets the callback or is destroyed the
    // shared_ptr ref-count drops to zero and the thunk is deleted.
}

// -----------------------------------------------------------------------------

void DirectCallerClient::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state)  {
  DirectPeer::OnIceConnectionChange(new_state);
  if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed ||
      new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected) {
    APP_LOG(AS_INFO) << "DirectCallerClient: ICE connection failed/disconnected, attempting fallback if available";
    ResetCallStartedFlag();
  }
}

// -----------------------------------------------------------------------------

