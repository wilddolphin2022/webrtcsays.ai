#include "wsock.h"
#include "direct.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <functional>
#include <vector>
#include <netdb.h>      // getaddrinfo for STUN lookup
#include <cstdint>
#include <cstdlib>
#include <atomic>

// Symbol visibility attributes for shared library export
#if defined(__GNUC__) || defined(__clang__)
#define EXPORT_API __attribute__((visibility("default")))
#else
#define EXPORT_API
#endif

// Forward declarations
namespace Json {
    class Value;
}

// Callback types for peer events
using PeerJoinedCallback = std::function<void(const std::string& peer_id)>;
using PeerListCallback = std::function<void(const std::vector<std::string>& peer_ids)>;
using OfferReceivedCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
using AnswerReceivedCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
using IceCandidateReceivedCallback = std::function<void(const std::string& peer_id, const std::string& candidate)>;
using HelloReceivedCallback = std::function<void(const std::string& peer_id)>;
using UserListReceivedCallback = std::function<void(const std::vector<std::string>& user_ids)>;

// Callback for raw INVITE lines (including JSON payload)
using InviteReceivedCallback = std::function<void(const std::string& message)>;
using WaitingReceivedCallback = std::function<void(const std::string& message)>;

// Callback for receiving a direct IP:port address for a peer
using AddressReceivedCallback = std::function<void(const std::string& user_id,
                                                  const std::string& ip,
                                                  int port)>;

class DirectClient {
public:
    DirectClient(const Options& opts);
    ~DirectClient();

    bool connectToSignalingServer(const std::string& server_host, const std::string& server_port);
    bool startTcpServer(int listen_port);  // For callee to act as TCP server
    void disconnect();
    void registerWithRoom(const std::string& room_name);
    bool isConnected() const;
    bool isRegistered() const;
    
    // Callback setters
    void setPeerJoinedCallback(PeerJoinedCallback callback) { peer_joined_callback_ = callback; }
    void setPeerListCallback(PeerListCallback callback) { peer_list_callback_ = callback; }
    void setOfferReceivedCallback(OfferReceivedCallback callback) { offer_received_callback_ = callback; }
    void setAnswerReceivedCallback(AnswerReceivedCallback callback) { answer_received_callback_ = callback; }
    void setIceCandidateReceivedCallback(IceCandidateReceivedCallback callback) { ice_candidate_received_callback_ = callback; }
    void setHelloReceivedCallback(HelloReceivedCallback callback) { hello_received_callback_ = callback; }
    void setAddressReceivedCallback(AddressReceivedCallback callback) { address_received_callback_ = callback; }
    void setUserListReceivedCallback(UserListReceivedCallback callback) { user_list_received_callback_ = callback; }
    void setInviteReceivedCallback(InviteReceivedCallback callback) { invite_received_callback_ = std::move(callback); }
    void setWaitingReceivedCallback(WaitingReceivedCallback callback) { waiting_received_callback_ = std::move(callback); }

    // Indicates whether this client is currently busy (active call)
    void setIsBusyCallback(const std::function<bool()>& cb) { is_busy_callback_ = cb; }
  
    // Message sending methods
    bool sendOffer(const std::string& target_peer_id, const std::string& sdp);
    bool sendAnswer(const std::string& target_peer_id, const std::string& sdp);
    bool sendIceCandidate(const std::string& target_peer_id, const std::string& candidate);
    bool sendAddress(const std::string& user_id, const std::string& ip, int port);
    bool sendInit();
    bool sendBye();
    bool sendCancel();
    bool sendHelloToUser(const std::string& target_user_id);  // Send HELLO to specific user
    bool requestUserList();  // Request list of users from signaling server
    
    // Called by owner (callee) to ensure ADDRESS line is replayed after reconnect
    void setAddressToPublish(const std::string& addr) { pending_address_ = addr; }
    void setRoomToPublish(const std::string& room) { pending_room_ = room; }
    
    // Temporarily stop / restart WebSocket listening without closing the TCP
    // connection so the peer stays registered on the signalling server.
    void pause();   // Stop keep-alive & async_read
    void resume();  // Restart them

    std::shared_ptr<WebSocketClient> &ws_client() { return ws_client_; }
protected:
    std::shared_ptr<WebSocketClient> ws_client_;
    std::unique_ptr<rtc::Thread> network_thread_;
    std::string jwt_token_;
    Options opts_;
    bool connected_;
    bool registered_;
    std::string pending_room_;
    std::atomic<bool> shutting_down_{false};

    // Callbacks for peer events
    PeerJoinedCallback peer_joined_callback_;
    PeerListCallback peer_list_callback_;
    OfferReceivedCallback offer_received_callback_;
    AnswerReceivedCallback answer_received_callback_;
    IceCandidateReceivedCallback ice_candidate_received_callback_;
    HelloReceivedCallback hello_received_callback_;
    AddressReceivedCallback address_received_callback_;
    UserListReceivedCallback user_list_received_callback_;
    InviteReceivedCallback invite_received_callback_;
    WaitingReceivedCallback waiting_received_callback_;
    std::function<bool()> is_busy_callback_;
    
    std::function<void(const std::string&)> default_ws_handler_;
    
private:
    std::string getJwtToken(const std::string& server_host, int server_port, 
                           const std::string& userId, const std::string& password);
    void handleProtocolMessage(const std::string& message);
    void handleWebSocketMessage(const std::string& message);
    void handleSocketIOEvent(const std::string& json_content);
    void handlePeerJoined(const Json::Value& message);
    void handlePeerList(const Json::Value& message);
    void handleOffer(const Json::Value& message);
    void handleAnswer(const Json::Value& message);
    void handleIceCandidate(const Json::Value& message);
    void handleError(const Json::Value& message);

    std::string saved_room_;        // last room we registered with
    std::string pending_address_;   // full ADDRESS:... line to replay after reconnect
};

// DirectCallerClient - Initiates calls to other peers by name using signaling server
class EXPORT_API DirectCallerClient : public DirectCaller {
protected:
    // For signaling server communication
    std::unique_ptr<DirectClient> signaling_client_;
    std::atomic<bool> shutting_down_{false};

private:
    bool initialized_ = false;
    std::string resolved_target_ip_;
    int resolved_target_port_ = 0;
    rtc::Thread* owner_thread_ = nullptr; // Thread where the object was created
    UserListReceivedCallback user_list_callback_;

    // True once we've launched the initial WebRTC dial. Prevents retry on
    // secondary (private) ADDRESS lines when a public address was already
    // tried.
    std::atomic<bool> call_started_{false};

    // Fallback address storage moved to DirectPeer base (pending_ip_, pending_port_)

public:
    // Alternate constructor taking fully-populated Options directly
    explicit DirectCallerClient(const Options& opts);
    ~DirectCallerClient();

    bool Initialize();
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    void RunOnBackgroundThread();
    bool WaitUntilConnectionClosed(int timeout_ms = 5000);
    
    // Set target user to call by name
    void SetTargetUser(const std::string& target_user_id) { opts_.target_name = target_user_id; }
    
    // Add SetUserListCallback and RequestUserList to DirectCallerClient public section
    void SetUserListCallback(UserListReceivedCallback callback) { signaling_client_->setUserListReceivedCallback(callback); }
    bool RequestUserList();

    // Allow external classes (e.g., DirectCaller base implementation) to
    // clear the internal call_started_ flag after a completed call so that
    // the next ADDRESS message can trigger a fresh dial.
    void ResetCallStartedFlag() override {
        call_started_ = false;
        // If we previously queued a fallback ADDRESS while busy, dial it now.
        if (!pending_ip_.empty()) {
            call_started_ = true;  // prevent further queuing
            std::string ip  = pending_ip_;
            int         prt = pending_port_;
            pending_ip_.clear();
            pending_port_ = 0;
            std::thread([this, ip, prt] { initiateWebRTCCall(ip, prt); }).detach();
        }
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    
private:
    void onPeerJoined(const std::string& peer_id);
    void onPeerAddressResolved(const std::string& peer_id, const std::string& ip, int port);
    // initiateWebRTCCall now implemented in DirectPeer base
    void onAnswerReceived(const std::string& peer_id, const std::string& sdp);
    void onIceCandidateReceived(const std::string& peer_id, const std::string& candidate);
};

// DirectCalleeClient - Accepts incoming calls by name using signaling server  
class EXPORT_API DirectCalleeClient : public DirectCallee {
protected:
    std::unique_ptr<DirectClient> signaling_client_;  // For signaling server communication
    std::atomic<bool> shutting_down_{false};
    std::string active_peer_id_;

private:
    bool initialized_ = false;
    bool listening_ = false;

public:
    explicit DirectCalleeClient(const Options& opts);
    ~DirectCalleeClient();

    // Forward signalling-line (LLAMA:, WHISPER:, etc.) to caller via WebSocket.
    bool SendMessage(const std::string& message) override;

    bool Initialize();
    bool StartListening();
    void StopListening();
    bool IsListening() const;
    bool IsConnected() const;
    void RunOnBackgroundThread();
    bool WaitUntilConnectionClosed(int timeout_ms = 5000);
    void ResetConnectionClosedEvent();
    void SignalQuit();
    
private:
    void onIncomingCall(const std::string& peer_id, const std::string& sdp);
    // Helper to generate an SDP answer and send it via signaling server
    void createAndSendAnswer(const std::string& peer_id);
    void setupWebRTCListener();
    void publishAddressToSignalingServer();
    void onIceCandidateReceived(const std::string& peer_id, const std::string& candidate);

    // Send our ICE candidates through the signaling server (overrides DirectPeer behaviour)
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
};

// ---------------------------------------------------------------------------
//  C-friendly wrapper: allows Objective-C / Swift etc. to register a callback
//  without constructing a std::function object in the caller's binary – this
//  prevents libc++ ABI mismatches between the framework and the application.
//
//  The wrapper lives inside the framework (compiled with the same tool-chain
//  as the rest of the C++ code) and internally converts the plain C callback
//  into the std::function that DirectClient expects.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DirectUserListCallbackC)(const char** users,
                                        int          count,
                                        void*        context);

// Registers a C-style callback that will be invoked whenever a fresh list of
// user IDs is received from the signaling server. The callback is executed on
// the signaling / network thread, therefore the callee must forward to the UI
// thread if UI work is required.
EXPORT_API void DirectCallerClient_SetUserListCallbackC(
        DirectCallerClient*       client,
        DirectUserListCallbackC   callback,
        void*                     context);

#ifdef __cplusplus
} // extern "C"
#endif

// -------------------------------------------------------------
//  Minimal RFC-5389 binding-request helper inlined for convenience
//  Returns true on success and fills out_ip / out_port with the
//  server-reflexive (public) address that the NAT assigned to
//  the given UDP socket.  Safe to call multiple times; the function
//  is defined as `inline` to avoid multiple-definition errors when
//  this header is included from several translation units.
// -------------------------------------------------------------

inline bool stun_discover(int             sockfd,
                          const char*     stun_host,
                          uint16_t        stun_port,
                          std::string&    out_ip,
                          uint16_t&       out_port) {
    // Build a STUN binding request (20-byte header, zero attributes)
    uint8_t pkt[20] = {0};
    pkt[1] = 0x01;                    // Type: Binding Request (0x0001)
    // Length = 0 → already zero
    pkt[4] = 0x21; pkt[5] = 0x12; pkt[6] = 0xA4; pkt[7] = 0x42;  // Magic cookie
    for (int i = 8; i < 20; ++i) pkt[i] = rand() & 0xff;         // Transaction ID

    // Resolve STUN host
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(stun_host, nullptr, &hints, &res) != 0 || !res) return false;
    sockaddr_in sa = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    sa.sin_port = htons(stun_port);
    freeaddrinfo(res);

    // Send request
    if (sendto(sockfd, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) return false;

    // Wait for response (1-second timeout)
    fd_set r; FD_ZERO(&r); FD_SET(sockfd, &r);
    timeval tv{1, 0};
    if (select(sockfd + 1, &r, nullptr, nullptr, &tv) <= 0) return false;

    // Receive packet
    uint8_t buf[512]; sockaddr_in from{}; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n < 20) return false;

    // Parse STUN attributes for XOR-MAPPED-ADDRESS (0x0020)
    size_t pos = 20;
    const uint32_t cookie = 0x2112A442;
    while (pos + 4 <= static_cast<size_t>(n)) {
        uint16_t type = (buf[pos] << 8) | buf[pos + 1];
        uint16_t len  = (buf[pos + 2] << 8) | buf[pos + 3];
        if (type == 0x0020 && len >= 8 && pos + 4 + len <= static_cast<size_t>(n)) {
            uint8_t fam = buf[pos + 5];
            uint16_t xport = (buf[pos + 6] << 8) | buf[pos + 7];
            if (fam == 0x01) {  // IPv4
                uint32_t xip;
                std::memcpy(&xip, buf + pos + 8, 4);
                xip ^= htonl(cookie);
                out_ip = inet_ntoa(*reinterpret_cast<in_addr*>(&xip));
                out_port = xport ^ static_cast<uint16_t>(cookie >> 16);
                return true;
            }
        }
        pos += 4 + len;
    }
    return false;
}

// ---------------------------------------------------------------------------
