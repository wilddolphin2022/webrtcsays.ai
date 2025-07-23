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

#ifndef WEBRTC_DIRECT_DIRECT_H_
#define WEBRTC_DIRECT_DIRECT_H_

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include <atomic>
#include <functional>
#include <chrono>

#include <absl/memory/memory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/environment/environment_factory.h>
#include <api/jsep.h>
#include <api/peer_connection_interface.h>
#include <api/rtc_error.h>
#include <api/rtp_receiver_interface.h>
#include <api/rtp_sender_interface.h>
#include <api/rtp_transceiver_interface.h>
#include <api/media_stream_interface.h>
#include <api/data_channel_interface.h>
#include <api/audio/audio_mixer.h>
#include <api/audio/audio_processing.h>
#include <api/scoped_refptr.h>
#include <api/sequence_checker.h>
#include <api/set_local_description_observer_interface.h>
#include <api/set_remote_description_observer_interface.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/task_queue/task_queue_base.h>
#include <api/video/video_frame.h>            // Needed for ConsoleVideoRenderer
#include <api/video/video_sink_interface.h> // Needed for ConsoleVideoRenderer
#include <api/video_codecs/video_decoder_factory_template.h>
#include <api/video_codecs/video_decoder_factory_template_dav1d_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_open_h264_adapter.h>
#include <api/video_codecs/video_encoder_factory_template.h>
#include <api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h>
#include <api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h>
#include <api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h>
#include <api/video_codecs/video_encoder_factory_template_open_h264_adapter.h>
#include <modules/audio_device/include/audio_device.h>
#include <modules/audio_device/audio_device_impl.h>
#include <p2p/base/basic_packet_socket_factory.h>
#include <p2p/base/port_allocator.h>
#include <p2p/client/basic_port_allocator.h>
#include <pc/peer_connection.h>
#include <pc/peer_connection_factory.h>
#include <pc/session_description.h>
#include <pc/test/mock_peer_connection_observers.h>
#include <rtc_base/async_packet_socket.h>
#include <rtc_base/async_tcp_socket.h>
#include <rtc_base/checks.h>
#include <rtc_base/logging.h>
#include <rtc_base/net_helpers.h>
#include <rtc_base/network.h>
#include <rtc_base/physical_socket_server.h>
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/rtc_certificate.h>
#include <rtc_base/socket.h>
#include <rtc_base/socket_address.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/ssl_certificate.h>
#include <rtc_base/ssl_identity.h>
#include <rtc_base/thread.h>
#include <rtc_base/virtual_socket_server.h>
#include <rtc_base/third_party/sigslot/sigslot.h>
#include <rtc_base/event.h>
#include <rtc_base/system/rtc_export.h>

#include <system_wrappers/include/clock.h>
#include <system_wrappers/include/field_trial.h>
#include "test/vcm_capturer.h"

#include "video.h"

#if defined(WEBRTC_SPEECH_DEVICES) 
#include <modules/third_party/whillats/src/whillats.h>
#endif

namespace webrtc {
namespace test {
class VcmCapturer;
} // namespace test
} // namespace webrtc

class CameraFrameSink; // forward declaration

// Include TargetConditionals for TARGET_OS_IOS or TARGET_OS_OSX macros
#if defined(__APPLE__)
    #include <TargetConditionals.h>

  // Inject Obj-C forward declarations only:
  #if TARGET_OS_IOS && defined(__OBJC__)
  #import "sdk/objc/base/RTCVideoCapturer.h"
  #import "sdk/objc/components/renderer/metal/RTCMTLVideoView.h"
  #import "sdk/objc/api/peerconnection/RTCVideoTrack.h"
  #endif

#endif

#include "option.h"

#define LLAMA_NOTIFICATION_ENABLED 1

#ifdef WEBRTC_SPEECH_DEVICES
#include "modules/audio_device/speech/speech_audio_device_factory.h"
#endif  // WEBRTC_SPEECH_DEVICES

#ifdef __cplusplus

class LambdaCreateSessionDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit LambdaCreateSessionDescriptionObserver(
      std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>
                             desc)> on_success)
      : on_success_(on_success) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // Takes ownership of answer, according to CreateSessionDescriptionObserver
    // convention.
    on_success_(absl::WrapUnique(desc));
  }
  void OnFailure(webrtc::RTCError error) override {
  }

 private:
  std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface> desc)>
      on_success_;
};

class LambdaSetLocalDescriptionObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit LambdaSetLocalDescriptionObserver(
      std::function<void(webrtc::RTCError)> on_complete)
      : on_complete_(on_complete) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    on_complete_(error);
  }

 private:
  std::function<void(webrtc::RTCError)> on_complete_;
};

class LambdaSetRemoteDescriptionObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit LambdaSetRemoteDescriptionObserver(
      std::function<void(webrtc::RTCError)> on_complete)
      : on_complete_(on_complete) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    on_complete_(error);
  }

 private:
  std::function<void(webrtc::RTCError)> on_complete_;
};

class DIRECT_API DirectApplication : public webrtc::PeerConnectionObserver {
 public:
  DirectApplication(Options opts);
  ~DirectApplication() override;

  // Initialize threads and basic WebRTC infrastructure
  virtual bool Initialize();
  virtual bool CreatePeerConnection();

  // Common message handling
  virtual void HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr);

  virtual bool SendMessage(const std::string& message);

  // Utility: check whether a peer connection is currently not closed.
  // This can be used by callbacks (e.g. the Llama notification callback) to
  // avoid attempting to send when the peer has already disconnected.
  bool IsConnected() const { return peer_connection_ &&
    peer_connection_->peer_connection_state() !=
         webrtc::PeerConnectionInterface::PeerConnectionState::kClosed; }

  virtual void OnClose(rtc::AsyncPacketSocket* socket) {}

  // Run the application event loop
  void Run();
  void RunOnBackgroundThread();

  // Signal to quit the application
  virtual void SignalQuit() { 
    should_quit_ = true; 
    // Wake up all threads to process the quit signal by posting empty tasks
    if (network_thread_) network_thread_->PostTask([](){});
    if (worker_thread_) worker_thread_->PostTask([](){});
    if (signaling_thread_) signaling_thread_->PostTask([](){});
    if (main_thread_) main_thread_->PostTask([](){});
  }

  // Disconnect active connections without destroying core resources
  virtual void Disconnect();

  static void DIRECT_API rtcInitialize();
  static void DIRECT_API rtcCleanup();

  // Override WrapSocket to track created sockets
  rtc::Socket* WrapSocket(int s);
  rtc::Socket* CreateSocket(int family, int type);

  rtc::PhysicalSocketServer *pss() { return pss_; }
  rtc::BasicNetworkManager *network_manager() { return network_manager_; }

  // Allow host to inject a custom video source before starting
  virtual bool SetVideoSource(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source);

  // Allow host to inject a custom video sink before starting
  virtual bool SetVideoSink(std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink);

  // Add video track if source is available
  void AddVideoTrackIfSourceAvailable();
  void EnsureVideoSendActive();

#if defined(WEBRTC_IOS) && defined(__OBJC__)
  // Getter methods for video tracks (iOS only)
  RTC_OBJC_TYPE(RTCVideoTrack)* GetLocalVideoTrack();
  RTC_OBJC_TYPE(RTCVideoTrack)* GetRemoteVideoTrack();
#endif

  std::string remote_agent() { return remote_agent_; }

  // Local preference that we will announce in INIT. Default "audio".
  void setInitAgent(const std::string& agent) { local_agent_ = agent; }
  void useTextOnlyAgent() { local_agent_ = "text-only"; }
  std::string init_agent() const { return local_agent_; }

 protected:
  Options opts_;  // Store command line options
  bool is_caller() const { return opts_.mode == "caller"; }

  // Virtual method for derived classes to implement specific shutdown logic
  virtual void ShutdownInternal() {}

  rtc::PhysicalSocketServer *pss_ = new rtc::PhysicalSocketServer();
  rtc::BasicNetworkManager *network_manager_ = new rtc::BasicNetworkManager(pss());
    
  // Thread getters for derived classes
  rtc::Thread* signaling_thread() { return signaling_thread_.get(); }
  rtc::Thread* worker_thread() { return worker_thread_.get(); }
  rtc::Thread* network_thread() { return network_thread_.get(); }
  rtc::Thread* main_thread() { return main_thread_.get(); }
  rtc::Thread* ws_thread() { return ws_thread_.get(); }
  webrtc::PeerConnectionFactoryDependencies dependencies_ = {};
  rtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  std::unique_ptr<webrtc::TaskQueueBase, webrtc::TaskQueueDeleter> audio_task_queue_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  std::unique_ptr<rtc::BasicPacketSocketFactory> socket_factory_;

  rtc::scoped_refptr<rtc::RTCCertificate> certificate_;  // Store certificate
  std::unique_ptr<rtc::SSLCertificateStats> certificate_stats_;
  rtc::SSLCertificateStats* certificate_stats() const {
    return certificate_stats_.get();
  }
  rtc::RTCCertificate* certificate() const { return certificate_.get(); }
  void UpdateCertificateStats() {
    if (certificate_) {
      certificate_stats_ = certificate_->GetSSLCertificate().GetStats();
    }
  }

  // Video track and sink
  rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_ = nullptr;
  rtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_ = nullptr;
  // Video sink provided by host (e.g. Obj-C app)
  std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink_;
  rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_ = nullptr;
  
  std::vector<rtc::scoped_refptr<webrtc::EchoVideoTrackSource>> echo_sources_ = {};

  // Capability hint received from peer via INIT JSON ("text-only", "audio", ...)
  std::string remote_agent_ = "audio"; // default behaviour: send pre-synthesised audio unless peer says otherwise
  std::string local_agent_ = "audio";  // what we announce in our INIT
  
  void Cleanup();

  void QuitThreads() {
    should_quit_ = true;  // Add this member to DirectApplication class
    if (network_thread_)
      network_thread_->Quit();
    if (worker_thread_)
      worker_thread_->Quit();
    if (signaling_thread_)
      signaling_thread_->Quit();
    if (ws_thread_)
      ws_thread_->Quit();
    if (main_thread_)
        main_thread_->Quit();
  }

  // Message sequence tracking
  int ice_candidates_sent_ = 0;
  int ice_candidates_received_ = 0;
  int sdp_fragments_sent_ = 0;
  int sdp_fragments_received_ = 0;
  static constexpr int kMaxIceCandidates = 3;
  static constexpr int kMaxSdpFragments = 2;

  std::unique_ptr<rtc::AsyncTCPSocket> tcp_socket_;

  std::atomic<bool> should_quit_{false};
  // Ensures Cleanup() body runs only once even if called from multiple
  // threads (e.g. background thread and destructor).
  std::atomic<bool> cleaned_up_{false};

  static constexpr int kDebugNoEncryptionMode = true;

  // Implemented virtual methods from PeerConnectionObserver
  void OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override;
  void OnRemoveTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;

  //Not yet implemented virtual methods from PeerConnectionObserver
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {}
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {}
  void OnIceConnectionReceivingChange(bool receiving) override {}

 private:
  std::unique_ptr<rtc::Thread> main_thread_;

  // WebRTC threads
  std::unique_ptr<rtc::Thread> signaling_thread_;
  std::unique_ptr<rtc::Thread> worker_thread_;
  std::unique_ptr<rtc::Thread> network_thread_;
  std::unique_ptr<rtc::Thread> ws_thread_;
  std::unique_ptr<rtc::Thread> background_thread_;

  // Ensure methods are called on correct thread
  webrtc::SequenceChecker sequence_checker_;

  // VPN addresses
  std::vector<std::string> vpns_;

  // Track all sockets created by WrapSocket or CreateSocket
  std::vector<rtc::Socket*> tracked_sockets_;

  // Set video capturer from Objective-C
#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
  static WhillatsLlama* llama_;
  WhillatsSetResponseCallback llamaCallback_;
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED

#if defined(WEBRTC_IOS) && defined(__OBJC__)
 public:
  void SetVideoCapturer(RTC_OBJC_TYPE(RTCVideoCapturer)* capturer);
  void SetVideoRenderer(RTC_OBJC_TYPE(RTCMTLVideoView)* renderer);
#endif

};

class DIRECT_API DirectPeer : public DirectApplication {
 public:
  DirectPeer(Options opts);
  ~DirectPeer() override;

  void Start();

  void Mute() {
    if (audio_track_) {
      audio_track_->set_enabled(false);
    }
  }
  void Unmute() {
    if (audio_track_) {
      audio_track_->set_enabled(true);
    }
  }

  // Method for external logic to wait for the closed signal
  bool WaitUntilConnectionClosed(int give_up_after_ms);
  // Method to reset the event before a new connection attempt
  void ResetConnectionClosedEvent();
  
  // Override SignalQuit to also signal connection closed event
  void SignalQuit() override { 
    DirectApplication::SignalQuit(); 
    connection_closed_event_.Set(); // Force any waiting threads to wake up
  }

  // Override DirectApplication methods
  virtual void HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr) override;

  virtual bool SendMessage(const std::string& message) override;

  // PeerConnectionObserver implementation (inherited via DirectApplication)
  // Add overrides here if DirectApplication declares them virtual
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;

  // Event to signal complete connection closure
  rtc::Event connection_closed_event_;

  // Pending address for deferred call initiation (used by caller to queue
  // a fallback public address while waiting for a potentially better LAN
  // address).
  std::string pending_ip_;
  int         pending_port_ = 0;

  // Called by DirectCallerClient / DirectCalleeClient after a completed call
  // to clear queued state so the next ADDRESS can trigger a fresh dial.
  // Derived classes may override to perform additional work (e.g. clear
  // their own busy flags) but should invoke the base implementation.
  virtual void ResetCallStartedFlag();

  // Helper that dials the specified IP/port using DirectCaller::Connect and
  // launches the background processing loop.  Safe to call multiple times;
  // failures are logged and reported via the boolean return value.
  bool initiateWebRTCCall(const std::string& ip, int port);

  std::vector<std::unique_ptr<webrtc::test::VcmCapturer>> &capturers() { return capturers_; }
  std::vector<std::unique_ptr<CameraFrameSink>>           &sinks() { return sinks_; }

 protected:
  // When large OFFER/ANSWER SDP blobs are sent over the raw TCP signalling
  // channel they may arrive split across multiple packets.  We buffer the
  // fragments here until a complete SDP is received and successfully parsed.
  std::string pending_remote_sdp_;

  // Override the virtual shutdown method from DirectApplication
  void ShutdownInternal() override;

  webrtc::PeerConnectionInterface* peer_connection() const {
    return peer_connection_.get();
  }

  // Session description methods
  void SetRemoteDescription(const std::string& sdp);
  void AddIceCandidate(const std::string& candidate_sdp, int mline_index);

  // Drain any ICE candidates that were received before both local and remote
  // descriptions became available.
  void DrainPendingIceCandidates();
 
  rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;

  // Own the lifetime of active camera capturers/sinks so no globals are needed.
  std::vector<std::unique_ptr<webrtc::test::VcmCapturer>> capturers_;
  std::vector<std::unique_ptr<CameraFrameSink>>           sinks_;

  // Grant helper access to protected members for camera resource storage
  friend rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateCameraVideoSource(class DirectPeer*, const Options&);

  private:
  // ICE candidates that arrive before both local and remote descriptions are
  // set.  Each entry stores {mline_index, candidate_sdp}.
  std::vector<std::pair<int, std::string>> pending_ice_candidates_;

protected:
  // Observers are kept in members to extend their lifetime
  rtc::scoped_refptr<LambdaCreateSessionDescriptionObserver> create_session_observer_;
  rtc::scoped_refptr<LambdaSetLocalDescriptionObserver>      set_local_description_observer_;
  rtc::scoped_refptr<LambdaSetRemoteDescriptionObserver>     set_remote_description_observer_;
};

class DIRECT_API DirectCallee : public DirectPeer, public sigslot::has_slots<> {
 public:
  explicit DirectCallee(Options opts);
  virtual ~DirectCallee();

  bool StartListening();

 protected:
  // Signal handlers
  void OnNewConnection(rtc::AsyncListenSocket* socket, rtc::AsyncPacketSocket* new_socket);
  void OnMessage(rtc::AsyncPacketSocket* socket,
                 const unsigned char* data,
                 size_t len,
                 const rtc::SocketAddress& remote_addr);
  virtual void OnClose(rtc::AsyncPacketSocket* socket) override;
  void OnCancel(rtc::AsyncPacketSocket* socket);
  bool SendMessage(const std::string& message) override;

  // Override Peer callbacks to suppress teardown after graceful CANCEL
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;

  // Cleanly closes PeerConnection and defers heavy destruction to main thread.
  void ShutdownInternal() override;

  // Callee does not initiate connection, overrides base class
  bool Connect() { return false; }

  // Removed – now defined in DirectPeer base
  // virtual void ResetCallStartedFlag() {}
  
  int local_port_;
  std::unique_ptr<rtc::AsyncTcpListenSocket> listen_socket_;
  std::unique_ptr<rtc::AsyncTCPSocket> current_client_socket_; // Dedicated client socket

 private:
  // Flag set by OnCancel() to suppress the next closed-event coming from ICE.
  bool ignore_next_close_event_ = false;
};

class DIRECT_API DirectCaller : public DirectPeer {
 public:
  // NEW: Call state enumeration
  enum class State {
    Disconnected,
    Connecting,
    Connected,
    Failed
  };

  explicit DirectCaller(Options opts);
  ~DirectCaller() override;

  // Connect and send messages
  bool Connect();

  bool Connect(const char* ip, int port);
#if defined(__APPLE__)
  #if TARGET_OS_IOS || TARGET_OS_OSX
    bool ConnectWithBonjourName(const char* bonjour_name);
  #endif // #if TARGET_OS_IOS || TARGET_OS_OSX
#endif

  virtual void Disconnect() override;

  // Override to reset caller-specific handshake state (hello counter etc.)
  void ResetCallStartedFlag() override;

  // Getter for current remote address
  rtc::SocketAddress GetRemoteAddress() const { return remote_addr_; }

  // === State-change C callback API ===
  using StateChangeCB = void (*)(State new_state, void *ctx);
  void SetStateChangeCallback(StateChangeCB cb, void *ctx);

  // NEW: Get current state
  State GetState() const { return state_; }

 protected:
  // NEW: Update state
  void UpdateState(State new_state) {
    if (state_ == new_state) return;
    state_ = new_state;
    if (state_cb_) state_cb_(new_state, state_ctx_);
  }

 private:
  // Called when data is received on the socket
  void OnMessage(rtc::AsyncPacketSocket* socket,
                 const unsigned char* data,
                 size_t len,
                 const rtc::SocketAddress& remote_addr);

  // Called when connection is established
  void OnConnect(rtc::AsyncPacketSocket* socket);
  void OnClose(rtc::AsyncPacketSocket* socket) override {}
  
  // Retry logic for HELLO handshake in case the very first packet is lost
  void SendHelloWithRetry();        // Sends HELLO and schedules retries

  // Tracks whether WELCOME has been received so we can stop retrying
  bool welcome_received_ = false;
  // Current number of HELLO attempts performed for the active connection
  int hello_attempts_ = 0;

  // NEW: State tracking
  State state_ = State::Disconnected;
  std::function<void(State)> on_state_change_;

  static constexpr int kMaxHelloAttempts = 5;   // Give up after 5 attempts

  rtc::SocketAddress remote_addr_;

  // std::unique_ptr<rtc::AsyncTCPSocket> tcp_socket_;
  std::chrono::steady_clock::time_point last_disconnect_time_;

  // callback storage
  StateChangeCB state_cb_ = nullptr;
  void        * state_ctx_ = nullptr;
};

#endif  // __cplusplus
#endif  // WEBRTC_DIRECT_DIRECT_H_
