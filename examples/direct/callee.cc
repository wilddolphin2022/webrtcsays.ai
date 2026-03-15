/*
 *  (c) 2025, wilddolphin2022
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2022
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <arpa/inet.h>
#include <errno.h>  // For errno
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>  // For close

#include <chrono>
#include <cstdlib>
#include <cstring>  // For memset
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct.h"
#include "status.h"
#include "parser.h"

#if TARGET_OS_IOS || TARGET_OS_OSX
#include "bonjour.h"
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX

#if defined(__APPLE__) && !defined(SO_REUSEPORT)
#define SO_REUSEPORT 0x0200
#endif

using webrtc::PeerConnectionInterface;

// DirectCallee Implementation
DirectCallee::DirectCallee(Options opts) : DirectPeer(opts) {
  std::string ip;
  local_port_ = 0;
  ParseIpAndPort(opts.address, ip, local_port_);
}

DirectCallee::~DirectCallee() {
  if (tcp_socket_) {
    tcp_socket_->Close();
  }
  if (listen_socket_) {
    listen_socket_.reset();
  }
  video_sink_.reset();
}

bool DirectCallee::StartListening() {
  // Core listening setup logic as a callable
  auto listen_fn = [this]() -> bool {
    const int kMaxRetries = 10;
    const int kMaxRetryTimeMs = 30000; // 30 seconds total
    const int kInitialDelayMs = 100;   // Start with 100ms delay
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      // Check if we've exceeded the total timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time).count();
      if (elapsed >= kMaxRetryTimeMs) {
        RTC_LOG(LS_ERROR) << "Binding timeout exceeded (" << kMaxRetryTimeMs << "ms). Giving up.";
        break;
      }
      
      // Create raw socket
      int raw_socket = ::socket(AF_INET, SOCK_STREAM, 0);
      if (raw_socket < 0) {
        RTC_LOG(LS_ERROR) << "Failed to create socket, errno: "
                          << strerror(errno);
        return false;
      }

      // Set SO_REUSEADDR to allow address reuse
      int reuse = 1;
      if (::setsockopt(raw_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        RTC_LOG(LS_WARNING) << "Failed to set SO_REUSEADDR, errno: " << strerror(errno);
        // Continue anyway, this is not fatal
      }

#ifdef SO_REUSEPORT
      // macOS/IPV4 requires SO_REUSEPORT together with SO_REUSEADDR to rebind
      // to a port that is still in TIME_WAIT. This allows the listener to
      // restart immediately on the same port after a previous connection.
      if (::setsockopt(raw_socket, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        RTC_LOG(LS_WARNING) << "Failed to set SO_REUSEPORT, errno: " << strerror(errno);
      }
#endif

      // Setup server address
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(local_port_);
      addr.sin_addr.s_addr = INADDR_ANY;

      // Attempt to bind
      if (::bind(raw_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int bind_errno = errno;
        RTC_LOG(LS_WARNING) << "Bind attempt " << (attempt + 1) << "/" << kMaxRetries 
                           << " failed, errno: " << strerror(bind_errno) 
                           << " (port " << local_port_ << ")";
        ::close(raw_socket);
        
        // If not "Address already in use", don't retry
        if (bind_errno != EADDRINUSE) {
          RTC_LOG(LS_ERROR) << "Non-recoverable bind error: " << strerror(bind_errno);
          return false;
        }
        
        // Calculate delay with exponential backoff (100ms, 200ms, 400ms, 800ms, 1600ms, then cap at 2000ms)
        int delay_ms = kInitialDelayMs * (1 << std::min(attempt, 4));
        delay_ms = std::min(delay_ms, 2000); // Cap at 2 seconds
        
        // Make sure we don't exceed total timeout
        if (elapsed + delay_ms >= kMaxRetryTimeMs) {
          delay_ms = kMaxRetryTimeMs - elapsed;
          if (delay_ms <= 0) {
            RTC_LOG(LS_ERROR) << "No time left for retry delay. Giving up.";
            break;
          }
        }
        
        RTC_LOG(LS_INFO) << "Retrying bind in " << delay_ms << "ms...";
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        continue;
      }
      
      // Bind succeeded, continue with socket setup
      RTC_LOG(LS_INFO) << "Bind succeeded on attempt " << (attempt + 1) << " (port " << local_port_ << ")";

      // Retrieve the actual port assigned by the OS if port was 0
      socklen_t addrlen = sizeof(addr);
      if (getsockname(raw_socket, (struct sockaddr*)&addr, &addrlen) == 0) {
          local_port_ = ntohs(addr.sin_port);
      }

      // Listen
      if (::listen(raw_socket, 5) < 0) {
        RTC_LOG(LS_ERROR) << "Failed to listen, errno: " << strerror(errno);
        ::close(raw_socket);
        return false;
      }

      // Wrap the listening socket
      auto wrapped_socket = pss()->WrapSocket(raw_socket);
      if (!wrapped_socket) {
        RTC_LOG(LS_ERROR) << "Failed to wrap socket";
        ::close(raw_socket);
        return false;
      }

      listen_socket_ = std::make_unique<rtc::AsyncTcpListenSocket>(
          std::unique_ptr<rtc::Socket>(wrapped_socket));
      listen_socket_->SignalNewConnection.connect(this,
                                                  &DirectCallee::OnNewConnection);

      RTC_LOG(LS_INFO) << "Server listening on port " << local_port_;

      #if TARGET_OS_IOS || TARGET_OS_OSX
      // Bonjour advertisement
      if (opts_.bonjour && !opts_.bonjour_name.empty()) {
          if (AdvertiseBonjourService(opts_.bonjour_name, local_port_)) {
              RTC_LOG(LS_INFO) << "Bonjour advertised as '" << opts_.bonjour_name << "' on port " << local_port_;
          } else {
              RTC_LOG(LS_WARNING) << "Bonjour advertisement failed for '" << opts_.bonjour_name << "'";
          }
      }
      #endif // #if TARGET_OS_IOS || TARGET_OS_OSX
      return true;
    } // End of retry loop
    
    // If we get here, all retry attempts failed
    RTC_LOG(LS_ERROR) << "Failed to bind after " << kMaxRetries << " attempts within " 
                      << kMaxRetryTimeMs << "ms timeout. Giving up.";
    return false;
  };
  // If already on network thread, run directly to avoid BlockingCall DCHECK
  if (network_thread()->IsCurrent()) {
    return listen_fn();
  }
  network_thread()->Restart();
  return network_thread()->BlockingCall(std::move(listen_fn));
}

void DirectCallee::OnNewConnection(rtc::AsyncListenSocket* listen_socket,
                                   rtc::AsyncPacketSocket* new_socket) {
  RTC_LOG(LS_INFO) << "New connection received";

  if (!new_socket) {
    RTC_LOG(LS_ERROR) << "New socket is null";
    return;
  }

  // Use a pointer to the specific socket for this connection
  rtc::AsyncTCPSocket* current_client_socket =
      static_cast<rtc::AsyncTCPSocket*>(new_socket);
  RTC_LOG(LS_INFO) << "Connection accepted from "
                   << current_client_socket->GetRemoteAddress().ToString()
                   << " on socket (" << new_socket << ")";

  // If we are already in an active call (peer_connection_ or an existing
  // tcp_socket_), politely reject the new connection with "486 Busy Here"
  if (this->peer_connection_ || (tcp_socket_ && tcp_socket_.get() != nullptr)) {
    std::string busy_msg = StatusCodes::kBusyHere;
    current_client_socket->Send(busy_msg.c_str(), busy_msg.size(), rtc::PacketOptions());
    current_client_socket->Close();
    return; // keep existing call intact
  }

  // No active call – proceed to accept this socket.
  tcp_socket_.reset(current_client_socket);

  // Register callback on the specific socket for this connection
  current_client_socket->RegisterReceivedPacketCallback(
      // Capture the specific socket pointer for this callback instance
      [this, current_client_socket](rtc::AsyncPacketSocket* socket_param,
                                    const rtc::ReceivedPacket& packet) {
        // Verify the callback is running for the socket it was registered on
        if (socket_param != current_client_socket) {
          RTC_LOG(LS_WARNING)
              << "Received packet on unexpected socket instance. Ignoring.";
          return;
        }
        RTC_LOG(LS_INFO) << "Received packet of size: "
                         << packet.payload().size() << " on socket "
                         << current_client_socket;
        // Pass the correct socket instance (current_client_socket) to OnMessage
        OnMessage(
            current_client_socket,
            reinterpret_cast<const unsigned char*>(packet.payload().data()),
            packet.payload().size(), packet.source_address());
      });
  current_client_socket->SubscribeCloseEvent(
      this,  // use `this` or any stable tag
      [this](rtc::AsyncPacketSocket* socket, int err) {
        RTC_LOG(LS_INFO) << "Wire‐level socket closed, err=" << err << ". Initiating Disconnect().";

        // Ensure we tear down media etc. to stop heavy workers like Llama.
        // Do this on the main thread (same choice as Disconnect()) to avoid threading issues.
        this->main_thread()->PostTask([self = this]() {
          self->Disconnect();
        });

        // Signal any waiters so higher-level loops can restart.
        connection_closed_event_.Set();
      });

  RTC_LOG(LS_INFO) << "Callback registered for incoming messages on socket "
                   << current_client_socket;
}

void DirectCallee::OnMessage(rtc::AsyncPacketSocket* socket,
                             const unsigned char* data,
                             size_t len,
                             const rtc::SocketAddress& remote_addr) {
  // Quick log of incoming message (up to 127 chars)
  char bufLog[128];
  size_t cnt = len < sizeof(bufLog) - 1 ? len : sizeof(bufLog) - 1;
  memcpy(bufLog, data, cnt);
  bufLog[cnt] = '\0';
  RTC_LOG(LS_INFO) << "Callee received: " << bufLog << " from "
                   << remote_addr.ToString();

  std::string message(reinterpret_cast<const char*>(data), len);
  // Use prefix match to be tolerant of combined packets
  if (message.rfind(Msg::kHello, 0) == 0) {
    // Determine callee state to pick correct status code
    //  - 486 Busy Here: already in an active PeerConnection
    //  - 200 OK: ready to accept call
    //  - 480 Temporarily Unavailable: not currently listening (should not occur on this path)

    if (peer_connection_) {
      // We are already in a call – reject with Busy status.
      SendMessage(StatusCodes::kBusyHere);
    } else if (!listen_socket_) {
      // Should not happen because we're handling a TCP connection, but keep for completeness.
      SendMessage(StatusCodes::kTemporarilyUnavailable);
    } else {
      // Ready – acknowledge.
      SendMessage(StatusCodes::kOk);
    }
  } else if (message.rfind(Msg::kBye, 0) == 0) {
    SendMessage(StatusCodes::kOk);
    // Schedule a graceful teardown of the current PeerConnection so that the
    // callee immediately becomes available for a new incoming call.  We post
    // this task to the main thread to avoid blocking the network thread that
    // delivered the BYE packet.
    main_thread()->PostTask([self = this]() {
      self->ShutdownInternal();
    });
    // Skipping synchronous shutdown for BYE to avoid blocking
  } else if (message.rfind(Msg::kCancel, 0) == 0) {
    RTC_LOG(LS_INFO) << "Received CANCEL from " << remote_addr.ToString()
                     << ". Restarting listener.";
    OnCancel(socket);
  } else {
    // Forward other messages to default handler
    // Malformed JSON? – respond with 400 once.
    std::string cmd;
    Json::Value params;
    if (!ParseMessageLine(std::string((const char*)data, len), cmd, params)) {
        SendMessage(StatusCodes::kBadRequest);
    }
    HandleMessage(socket, message, remote_addr);
  }
}

void DirectCallee::OnCancel(rtc::AsyncPacketSocket* socket) {
  if (socket != tcp_socket_.get()) {
    RTC_LOG(LS_WARNING) << "Received CANCEL on an unexpected socket.";
    return;
  }

  // Clear any per-call busy flags so the callee becomes immediately
  // available for follow-up calls (e.g., when a queued address is waiting
  // to be dialed by a DirectCalleeClient subclass).
  ResetCallStartedFlag();      // let derived class clear busy flag / dial queued address

  // Send explicit BYE in response to CANCEL so the caller can distinguish it
  // from the initial 200 OK used in the HELLO handshake.
  SendMessage(Msg::kBye);

  // Gracefully shut down the active PeerConnection (if any) so we release all
  // WebRTC resources tied to the just-finished call, but keep the listening
  // socket open so that we can accept the next incoming connection on the
  // same TCP port without having to restart the whole DirectCalleeClient.
  ShutdownInternal();

  // Close and dispose of the per-connection TCP socket on the main thread in
  // order to avoid blocking the network thread that delivered the packet.  We
  // intentionally leave the listening socket (listen_socket_) intact so that
  // the operating system continues to accept new connections on the already
  // published port.  This eliminates the short time window during which a
  // follow-up caller could observe "connection refused" before the callee has
  // had a chance to restart and re-advertise a (potentially different) port.
  auto old_socket = std::move(tcp_socket_);
  main_thread()->PostTask([s = std::move(old_socket)]() mutable {
    // unique_ptr s will be destroyed here when the task runs.
  });

  // NOTE: We intentionally do NOT signal connection_closed_event_ here.  The
  // event is used by the outer application loop to decide when to tear down
  // and recreate the entire DirectCalleeClient instance.  For a simple CANCEL
  // (normal end-of-call) we can handle the clean-up in-place and stay ready
  // for the next call immediately, avoiding any avoidable downtime.

  // Mark that we handled a graceful CANCEL so that the forthcoming ICE state
  // transitions (which will go to "closed") don't propagate to the outer
  // control-loop via connection_closed_event_.
  ignore_next_close_event_ = true;

  worker_thread()->PostTask([this]() {
    if (audio_device_module_) {
      audio_device_module_->StopPlayout();
      audio_device_module_->StopRecording();
      audio_device_module_->Terminate();
      audio_device_module_ = nullptr;
      dependencies_.adm   = nullptr;
    }
  });

  // Notify outer control loop that the connection has ended so it can
  // recreate a fresh DirectCalleeClient (and therefore a brand-new listening
  // socket/port).  This avoids connectivity issues some NATs exhibit when we
  // keep re-using the same port across calls.
  // connection_closed_event_.Set();
}

void DirectCallee::OnClose(rtc::AsyncPacketSocket* socket) {
  RTC_LOG(LS_INFO) << "Callee socket closed";
}

bool DirectCallee::SendMessage(const std::string& message) {
  RTC_LOG(LS_INFO) << "Callee SEND: " << message;

  return DirectPeer::SendMessage(message);
}

// Override to optionally suppress the global connection-closed signal when we
// have handled a graceful CANCEL (ignore_next_close_event_ == true).
void DirectCallee::OnIceConnectionChange(PeerConnectionInterface::IceConnectionState new_state) {
  if (ignore_next_close_event_ &&
      (new_state == PeerConnectionInterface::kIceConnectionClosed ||
       new_state == PeerConnectionInterface::kIceConnectionDisconnected ||
       new_state == PeerConnectionInterface::kIceConnectionFailed)) {
    RTC_LOG(LS_INFO) << "Ignoring IceConnectionChange('closed') triggered by graceful CANCEL.";
    ignore_next_close_event_ = false; // consume flag
    return; // Do NOT propagate to base, which would set connection_closed_event_
  }

  // Fallback to default behaviour
  DirectPeer::OnIceConnectionChange(new_state);
}

void DirectCallee::ShutdownInternal() {
  DirectPeer::ShutdownInternal();
}
