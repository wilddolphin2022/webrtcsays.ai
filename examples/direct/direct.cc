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

#include "rtc_base/network.h"
#include "rtc_base/ip_address.h"
#include <unistd.h> // For usleep
#include <vector>
#include <string>
#include <algorithm>
#include <regex>
#include <atomic>
#include <map>
#include <sstream>
#include <memory>
#include <cstdlib>
#include "rtc_base/time_utils.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"

#include "direct.h"
#include "option.h"
#include "video.h"
#include "status.h"

// String split from option.cc
std::vector<std::string> stringSplit(std::string input, std::string delimiter);

void DirectApplication::rtcInitialize() {
  rtc::InitializeSSL();
}

void DirectApplication::rtcCleanup() {
  rtc::CleanupSSL();
}

rtc::IPAddress IPFromString(absl::string_view str) {
  rtc::IPAddress ip;
  RTC_CHECK(rtc::IPFromString(str, &ip));
  return ip;
}

#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
WhillatsLlama* DirectApplication::llama_ = nullptr;

void llamaCallback(bool success, const char* response, void* user_data) {
  if (success && response && *response && user_data) {
    DirectApplication* app = static_cast<DirectApplication*>(user_data);
    if(app) {
      // Safely handle the language string
      std::string language = webrtc::SpeechAudioDeviceFactory::GetLanguage();
      if (language.empty()) {
        language = "en";  // Default to English if empty
      }
      
      if (app->remote_agent() == "text-only") {
        // Remote expects text notifications; send LLAMA message
        if (app->IsConnected()) {
          std::string message;
          message.append("LLAMA:[").append(language).append("]").append(response);
          app->SendMessage(message);
        }
      } else if(app->remote_agent() == "audio") { // "audio" or any other value: play TTS locally so audio is transmitted via WebRTC
        webrtc::SpeechAudioDeviceFactory::SpeakText(response, language);
      } else {
        RTC_LOG(LS_WARNING) << "Llama notification arrived but peer is no longer connected. Dropping message: [" << language << "]" << response;
      }
    }
  }
}
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED

// DirectApplication Implementation
DirectApplication::DirectApplication(Options opts)
  : opts_(opts)
#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
    , llamaCallback_(llamaCallback, this)
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
{
  // Threads will be created in Initialize() to support full teardown/re-init
  peer_connection_factory_ = nullptr;
}

void DirectApplication::Cleanup() {
  // Ensure this method executes only once.
  if (cleaned_up_.exchange(true)) {
    RTC_LOG(LS_INFO) << "Cleanup already performed – skipping.";
    return;
  }

  // Explicitly close and release PeerConnection via Shutdown before stopping threads
  ShutdownInternal();

  // Remove sink before closing the connection - handle both local and remote tracks
  if (video_sink_) {
      auto remove_sinks_lambda = [this]() {
          if (video_sink_) {
              if (video_track_) {
                  RTC_LOG(LS_INFO) << "Removing video sink from local track (Cleanup).";
                  video_track_->RemoveSink(video_sink_.get());
              }
              if (remote_video_track_) {
                  RTC_LOG(LS_INFO) << "Removing video sink from remote track (Cleanup).";
                  remote_video_track_->RemoveSink(video_sink_.get());
              }
          }
      };

      // Ensure we run on signaling thread.
      if (signaling_thread_ && rtc::Thread::Current() != signaling_thread_.get()) {
          signaling_thread_->BlockingCall(remove_sinks_lambda);
      } else {
          remove_sinks_lambda();
          // Now destroy the sink to ensure no pending callbacks after teardown.
          video_sink_.reset();
          RTC_LOG(LS_INFO) << "Video sink destroyed after detaching.";
      }
  }

  // Release video source to avoid callbacks after cleanup
  if (video_source_) {
      video_source_ = nullptr;
      RTC_LOG(LS_INFO) << "Video source released after cleanup.";
  }

  // Explicitly terminate and release the AudioDeviceModule (ADM) – doing this
  // on the worker thread that owns the ADM prevents race-conditions with the
  // audio callback threads that would otherwise access freed memory and crash
  // (observed as segfaults right after "failed to retrieve the playout delay").
  auto adm_shutdown_lambda = [this]() {
      if (audio_device_module_) {
        // Ensure playout/recording are stopped first.  Terminate() may DCHECK
        // if audio is still active (observed segfault in cleanup path).
        audio_device_module_->StopPlayout();
        audio_device_module_->StopRecording();

        RTC_LOG(LS_INFO) << "Terminating AudioDeviceModule before destruction";
        // Best-effort: ignore return value – some back-ends may not implement
        // Terminate() and simply return an error code.
        audio_device_module_->Terminate();
      }
      peer_connection_factory_ = nullptr;
      dependencies_.adm = nullptr;
      audio_device_module_ = nullptr;
  };

  if (worker_thread() && worker_thread()->IsCurrent()) {
      adm_shutdown_lambda();
  } else if (worker_thread()) {
      worker_thread()->BlockingCall(adm_shutdown_lambda);
  } else {
      // Fallback: if worker_thread_ is already gone execute directly.
      adm_shutdown_lambda();
  }

  // Ensure all pending tasks – including PeerConnectionFactoryProxy's
  // DestroyInternal() that was just queued – have actually run before we
  // proceed to stop / join the worker thread.  Post a no-op task and wait
  // for it to execute as a barrier.

  if (peer_connection_factory_) {
    rtc::Event released;

    if (signaling_thread_ && rtc::Thread::Current() != signaling_thread_.get()) {
      // Marshal the release to the signaling thread – that is the thread
      // where DestroyInternal() ultimately executes.
      signaling_thread_->PostTask([this, &released]() {
        peer_connection_factory_ = nullptr;
        released.Set();
      });
    } else {
      // Either no signaling thread or we are already on it.
      peer_connection_factory_ = nullptr;
      released.Set();
    }

    // Give more time for release to complete.
    if (!released.Wait(webrtc::TimeDelta::Seconds(5))) {
      RTC_LOG(LS_WARNING) << "Factory release timed out, forcing direct reset.";
      peer_connection_factory_ = nullptr;
    }
  }

  // Make sure the queued DestroyInternal() has really run before we shutdown
  // any threads: post a no-op barrier to the signaling thread and wait for it.

  if (signaling_thread_) {
    rtc::Event barrier;
    signaling_thread_->PostTask([&barrier]() { barrier.Set(); });
    // Give more time for DestroyInternal to complete.
    if (!barrier.Wait(webrtc::TimeDelta::Seconds(5))) {
      RTC_LOG(LS_WARNING) << "Signaling thread barrier timed out, tasks may not have completed.";
    }
  }

  // Ensure any cleanup tasks queued on the worker thread have run.
  if (worker_thread_) {
    rtc::Event wbarrier;
    worker_thread_->PostTask([&wbarrier]() { wbarrier.Set(); });
    // Give more time for any cleanup tasks.
    if (!wbarrier.Wait(webrtc::TimeDelta::Seconds(5))) {
      RTC_LOG(LS_WARNING) << "Worker thread barrier timed out, tasks may not have completed.";
    }
  }

  // Utility: stop and destroy a rtc::Thread from *another* thread to avoid
  // triggering the DCHECK(!IsCurrent()) inside rtc::Thread::Join().
  auto safe_stop_and_reset = [](std::unique_ptr<rtc::Thread>& th) {
    if (!th) {
      return;
    }
    // Always transfer ownership to a helper std::thread so the Stop()/Join()
    // call is guaranteed to execute on a *different* OS thread than the one
    // represented by the rtc::Thread instance, eliminating any possibility
    // of hitting the `!IsCurrent()` DCHECK inside rtc::Thread::Join().
    std::thread([owned = th.release()]() {
      if (owned) {
        owned->Quit();
        owned->Stop();  // safe: we are not the rtc::Thread we are stopping
        delete owned;
      }
    }).detach();
  };

  // Stop network and signaling first, worker last (after barriers).
  safe_stop_and_reset(network_thread_);
  safe_stop_and_reset(signaling_thread_);
  safe_stop_and_reset(worker_thread_);
  rtc::Thread::SleepMs(50);
  safe_stop_and_reset(ws_thread_);
  rtc::Thread::SleepMs(50);

  if (main_thread_) {
    if (rtc::Thread::Current() != main_thread_.get()) {
      safe_stop_and_reset(main_thread_);
    } else {
      // We ARE on main_thread_. Transfer ownership and shut it down from a
      // helper so the Join happens off-thread and avoids the DCHECK.
      rtc::Thread* owned = main_thread_.release();
      std::thread([owned]() {
        if (owned) {
          owned->Quit();
          owned->Stop();
          delete owned;
        }
      }).detach();
    }
  }

  // Clear remaining members
  socket_factory_.reset();
  
  if(pss_) {
    pss_->WakeUp();
    delete pss_;
  }
  if(network_manager_) {
    delete network_manager_;
  }
}

void DirectApplication::Disconnect() {
  if (rtc::Thread::Current() != main_thread_.get()) {
    main_thread_->PostTask([this]() { Disconnect(); });
    return;
  }

  // Close active connections and reset connection state without destroying core resources
  RTC_LOG(LS_INFO) << "Disconnecting active connections";

  // If a local video track exists ensure we detach the sink and release it on the signaling thread.
  if (video_track_) {
    RTC_LOG(LS_WARNING) << "Disconnecting: Video track exists, video transmission may stop. Track enabled: "
                        << (video_track_->enabled() ? "true" : "false");

    auto release_local_track = [this]() {
      // No sink was attached to the local track; just release the reference.
      video_track_ = nullptr;
    };

    if (signaling_thread_ && rtc::Thread::Current() != signaling_thread_.get()) {
      signaling_thread_->PostTask(release_local_track);
    } else {
      release_local_track();
    }

    // Safeguard: If source is live we intend to reuse it; that's fine.
  }

  // Close all tracked sockets to ensure they are removed from PhysicalSocketServer
  RTC_LOG(LS_INFO) << "Closing tracked sockets during disconnect";
  // Ensure any running Llama device is stopped before creating a new peer
#if defined(WEBRTC_SPEECH_DEVICES) && LLAMA_NOTIFICATION_ENABLED
  if (opts_.llama && llama_) {
    RTC_LOG(LS_INFO) << "Stopping existing Llama device during Disconnect()";
    llama_->stop();
    llama_ = nullptr;
  }
#endif
  for (auto* socket : tracked_sockets_) {
    if (socket) {
      RTC_LOG(LS_INFO) << "Closing socket during disconnect";
      socket->Close();
    }
  }
  tracked_sockets_.clear();

  // Reset connection-specific state
  if (peer_connection_) {
    RTC_LOG(LS_INFO) << "Closing peer connection, video transmission will stop if active.";

    // Close() must be called on signaling thread, then we need to release the
    // reference on the same thread so that the destructor executes where
    // WebRTC expects it.

    signaling_thread()->PostTask([this]() {
      if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;  // release on signaling thread
        RTC_LOG(LS_INFO) << "Peer connection closed and released on signaling thread (Disconnect).";
      }
    });

    // after peer_connection_->Close();
    // Stop audio playout/recording on the *worker_thread* where the ADM was
    // originally created.  Calling these methods from the wrong thread
    // triggers a strict thread checker inside WebRTC (see
    // audio_device_buffer.cc: TaskQueue doesn't match).  By executing the
    // calls on the owning thread we avoid the fatal RTC_DCHECK failure that
    // occurred during asynchronous teardown when the callee restarted.
    if (audio_device_module_ && worker_thread()) {
      worker_thread()->PostTask([this]() {
        audio_device_module_->StopPlayout();
        audio_device_module_->StopRecording();
      });
    }
    // don't destroy – just make sure it is in a clean state
  }
  
  // Preserve video pipeline components for reconnection but clear references
  if (remote_video_track_) {
    RTC_LOG(LS_INFO) << "Clearing remote video track reference for reconnection.";

    if (video_sink_) {
      auto remove_sink_lambda = [this]() {
        if (remote_video_track_ && video_sink_) {
          RTC_LOG(LS_INFO) << "Removing video sink from remote track on signaling thread.";
          remote_video_track_->RemoveSink(video_sink_.get());
        }
      };

      // Ensure we are on the signaling thread for WebRTC track operations.
      if (rtc::Thread::Current() != signaling_thread()) {
        signaling_thread()->BlockingCall(remove_sink_lambda);
      } else {
        remove_sink_lambda();
      }
    }

    remote_video_track_ = nullptr;
  }
  // peer_connection_factory_ = nullptr; // Keep factory alive for reconnection

  // Keep video source and sink alive for potential reconnection
  RTC_LOG(LS_INFO) << "Keeping video source and sink for potential reconnection.";

  // Reset message sequence counters for potential reconnection
  ice_candidates_sent_ = 0;
  ice_candidates_received_ = 0;
  sdp_fragments_sent_ = 0;
  sdp_fragments_received_ = 0;

  // Small delay to allow pending operations to complete
  rtc::Thread::SleepMs(100); // Delay (100ms) to ensure operations complete

  RTC_LOG(LS_INFO) << "Disconnected, ready for reconnection";

  // --- NEW BLOCK ---------------------------------------------------------
  if (audio_device_module_) {
    RTC_LOG(LS_INFO) << "Disconnect(): terminating and releasing ADM";
    // All ADM calls must happen on the same thread where the ADM was created
    // (our dedicated worker thread) otherwise WebRTC will DCHECK like:
    //   "TaskQueue doesn't match".
    worker_thread()->PostTask([this]() {
      if (!audio_device_module_) {
        return; // Already nulled from another path.
      }
      // Stop audio operations before termination to ensure no pending tasks
      audio_device_module_->StopPlayout();
      audio_device_module_->StopRecording();
      audio_device_module_->Terminate();
      audio_device_module_ = nullptr;
      dependencies_.adm   = nullptr;
    });
  }
  // -----------------------------------------------------------------------
}

void DirectApplication::Run() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  rtc::Event quit_event;

  // Set up quit handler on network thread
  network_thread_->PostTask([this, &quit_event]() {
    while (!should_quit_) {
      rtc::Thread::Current()->ProcessMessages(100);
    }
    quit_event.Set();
  });

  // Process messages on main thread until network thread signals quit
  while (!quit_event.Wait(webrtc::TimeDelta::Millis(0))) {
    rtc::Thread::Current()->ProcessMessages(100);
  }

  // Final cleanup only if quitting completely
  if (should_quit_) {
    Cleanup();
  } else {
    Disconnect(); // Otherwise just disconnect for potential reconnection
  }
}

void DirectApplication::RunOnBackgroundThread() {
  RTC_DCHECK(!rtc::Thread::Current()); // Ensure not on a WebRTC thread yet

  // If Initialize() has not been run yet, we have no main_thread_ to post to.
  if (!main_thread_) {
    RTC_LOG(LS_WARNING) << "RunOnBackgroundThread called before Initialize – ignoring.";
    return;
  }

  // Use main_thread_ instead of creating a new thread to avoid conflicts
  main_thread_->PostTask([this]() {
    while (!should_quit_) {
      main_thread_->ProcessMessages(100); // Process messages with 100ms timeout

    }
    if (should_quit_) {
      Cleanup();
    } else {
      Disconnect(); // Otherwise just disconnect for potential reconnection
    }
  });
}

DirectApplication::~DirectApplication() {
  if (!should_quit_)
    should_quit_ = true;
  Cleanup();

  // Even if Cleanup() was previously executed (and thus skipped), member
  // rtc::Thread instances may still be alive at this point because the object
  // can be destroyed on one of its own internal threads after asynchronous
  // operations.  Make absolutely sure they are torn down from a helper thread
  // so we never invoke Stop()/Join() on the same thread that is being joined.

  auto safe_stop_and_reset = [](std::unique_ptr<rtc::Thread>& th) {
    if (!th) return;
    std::thread([owned = th.release()]() {
      if (owned) {
        owned->Quit();
        owned->Stop();
        delete owned;
      }
    }).detach();
  };

  safe_stop_and_reset(network_thread_);
  safe_stop_and_reset(worker_thread_);
  safe_stop_and_reset(signaling_thread_);
  safe_stop_and_reset(ws_thread_);
  safe_stop_and_reset(main_thread_);
}

bool DirectApplication::Initialize() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  // Create and configure WebRTC threads
  main_thread_ = rtc::Thread::CreateWithSocketServer();
  main_thread_->socketserver()->SetMessageQueue(main_thread_.get());
  DirectThreadSetName(main_thread(), "Main");
  if (!main_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start main thread";
    return false;
  }
  ws_thread_ = rtc::Thread::Create();
  worker_thread_ = rtc::Thread::Create();
  signaling_thread_ = rtc::Thread::Create();
  network_thread_ = std::make_unique<rtc::Thread>(pss());
  network_thread_->socketserver()->SetMessageQueue(network_thread_.get());

  if (!worker_thread_->Start() || !signaling_thread_->Start() ||
      !network_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start threads";
    return false;
  }

  return true;
}


bool DirectApplication::CreatePeerConnection() {
  RTC_LOG(LS_INFO) << "Creating peer connection";

  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }

  // Log timing information to diagnose initialization order
  if (!video_source_ && opts_.video && is_caller()) {
    RTC_LOG(LS_WARNING) << "Video source not set yet for caller with video enabled. This may cause black frames. Ensure SetVideoSource is called before CreatePeerConnection.";
  } else if (video_source_) {
    RTC_LOG(LS_INFO) << "Video source present after peer-connection creation.";
  }

  // Create/get certificate if needed
  if (opts_.encryption && !certificate_) {
    certificate_ = DirectLoadCertificateFromEnv(opts_);
    certificate_stats_ = certificate_->GetSSLCertificate().GetStats();
    RTC_LOG(LS_INFO) << "Using certificate with fingerprint: "
                      << certificate_stats_->fingerprint << " and algorithm: "
                      << certificate_stats_->fingerprint_algorithm;
  }

  // Create a single task queue factory that will be used consistently
  std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory = 
      webrtc::CreateDefaultTaskQueueFactory();
  
  // Store the raw pointer for logging
  webrtc::TaskQueueFactory* task_queue_factory_ptr = task_queue_factory.get();
  
  // Create audio task queue - this will be used for audio operations
  audio_task_queue_ = task_queue_factory->CreateTaskQueue(
      "AudioTaskQueue", webrtc::TaskQueueFactory::Priority::NORMAL);
  
  RTC_LOG(LS_INFO) << "Created TaskQueueFactory: " << task_queue_factory_ptr
                  << " and AudioTaskQueue: " << audio_task_queue_.get();

  // Task queue factory setup
  dependencies_.network_thread = network_thread();
  dependencies_.worker_thread = worker_thread();
  dependencies_.signaling_thread = signaling_thread();

  webrtc::AudioDeviceModule::AudioLayer kAudioDeviceModuleType = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
  // Audio device module type
#ifdef WEBRTC_SPEECH_DEVICES
  if (opts_.whisper && opts_.llama) {
    webrtc::SpeechAudioDeviceFactory::SetLlamaEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetLlamaModelFilename(opts_.llama_model);
    webrtc::SpeechAudioDeviceFactory::SetLlavaMMProjFilename(opts_.llava_mmproj);
    //llama_ = nullptr; // uncomment to send message instead of audio 
#if LLAMA_NOTIFICATION_ENABLED
    llama_ = webrtc::SpeechAudioDeviceFactory::CreateWhillatsLlama(llamaCallback_);
#endif
  } 

  if (opts_.whisper && !opts_.whisper_model.empty()) {
    kAudioDeviceModuleType = webrtc::AudioDeviceModule::kSpeechAudio; 
    webrtc::SpeechAudioDeviceFactory::SetWhisperEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetWhisperModelFilename(opts_.whisper_model);
    webrtc::SpeechAudioDeviceFactory::SetLanguage(opts_.language);
  }

  if (opts_.llama && !opts_.llama_llava_yuv.empty()) {
    webrtc::SpeechAudioDeviceFactory::SetYuvFilename(opts_.llama_llava_yuv, 
      opts_.llama_llava_yuv_width, opts_.llama_llava_yuv_height);
  }
#endif // WEBRTC_SPEECH_DEVICES

  // Create the audio device module on the worker thread so its lifecycle runs there.
  rtc::Event adm_created;
  dependencies_.worker_thread->PostTask([this, kAudioDeviceModuleType, task_queue_factory_ptr, &adm_created]() {
    // Reset PCF and ADM references on worker thread for correct destruction.
    peer_connection_factory_ = nullptr;
    dependencies_.adm = nullptr;
    audio_device_module_ = nullptr;
    audio_device_module_ = webrtc::AudioDeviceModule::Create(
        kAudioDeviceModuleType,
        task_queue_factory_ptr);
    if (audio_device_module_) {
      RTC_LOG(LS_INFO) << "Audio device module created successfully on thread: "
                       << rtc::Thread::Current();

      // Attempt to initialize the ADM.  On head-less Linux servers PulseAudio
      // often isn't available which causes Init() to fail and WebRTC will
      // crash later when the voice engine asserts.  Detect this early and
      // transparently fall back to the dummy (no-audio) implementation so
      // that signalling and video can still work.
      int init_res = audio_device_module_->Init();
      if (init_res != 0) {
        RTC_LOG(LS_ERROR) << "Audio device module Init failed (" << init_res
                          << "), switching to DummyAudio layer";

        // Replace with dummy ADM – ignore return value, we tried our best.
        audio_device_module_ = webrtc::AudioDeviceModule::Create(
            webrtc::AudioDeviceModule::kDummyAudio,
            task_queue_factory_ptr);
        if (audio_device_module_) {
          audio_device_module_->Init();
          RTC_LOG(LS_INFO) << "Dummy audio device module created and initialized";
        } else {
          RTC_LOG(LS_ERROR) << "Failed to create Dummy audio device module";
        }
      }
    } else {
      RTC_LOG(LS_ERROR) << "Failed to create audio device module";
    }
    adm_created.Set();
  });

  // Wait for ADM creation to complete on the worker thread.
  adm_created.Wait(webrtc::TimeDelta::Seconds(1));
  if (!audio_device_module_) {
    RTC_LOG(LS_ERROR) << "Audio device module creation failed after task execution";
    return false;
  }

  dependencies_.adm = audio_device_module_;
  dependencies_.task_queue_factory = std::move(task_queue_factory);

  // PeerConnectionFactory creation
  peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
      dependencies_.network_thread,
      dependencies_.worker_thread,
      dependencies_.signaling_thread,
      dependencies_.adm,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      // Provide template-based video encoder and decoder factories
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>(),
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>(),      
      nullptr, // audio_mixer
      nullptr  // audio_processing
  );

  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return false; // Handle error appropriately
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionFactory::Options options = {};

  // Create/get certificate if needed and wire it into the configuration
  if (opts_.encryption) {
      RTC_LOG(LS_INFO) << "Encryption is enabled!";
      // Re-use the already loaded certificate (created during initialization)
      // to guarantee that the fingerprint we announce in the DTLS parameters
      // is exactly the one that will be presented during the TLS handshake.
      if(!certificate_) {
        // This can happen if CreatePeerConnection() is called before we had
        // a chance to load / generate the certificate in the constructor or
        // if encryption was toggled afterwards.  Ensure we have it.
        certificate_ = DirectLoadCertificateFromEnv(opts_);
        UpdateCertificateStats();
      }

      if(certificate_) {
        config.certificates.push_back(certificate_);
      } else {
        RTC_LOG(LS_ERROR) << "Failed to obtain certificate, WebRTC DTLS would not work";
      }
  } else {
      // WARNING! FOLLOWING CODE IS FOR DEBUG ONLY!
      options.disable_encryption = true;
      peer_connection_factory_->SetOptions(options);
      // END OF WARNING
  }

  // If a TURN server is configured prefer relay-only candidates. Otherwise
  // gather *only* server-reflexive candidates (skip host) so that peers won't
  // choose unroutable 192.168./10./172.* addresses even after an ICE restart.
#if 1
  // Allow host, srflx and relay candidates so local/LAN calls can connect via
  // direct host addresses.  Previously we used kNoHost which filtered them
  // out and prevented peer-to-peer on the same subnet.
  config.type = webrtc::PeerConnectionInterface::kAll;
#else
  config.type = webrtc::PeerConnectionInterface::kNoHost;  // srflx + relay only
#endif

    // Only set essential ICE configs
  config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
  
  // Only increase the connection timeout modestly to avoid validation issues
  config.ice_connection_receiving_timeout = 45000;  // 45 seconds (was 30)

  cricket::ServerAddresses stun_servers;
  stun_servers.insert(rtc::SocketAddress("stun.l.google.com", 19302));
  stun_servers.insert(rtc::SocketAddress("stun1.l.google.com", 19302));
  stun_servers.insert(rtc::SocketAddress("stun2.l.google.com", 19302));
  
  std::vector<cricket::RelayServerConfig> turn_servers;

  //if(opts_.turns.size()) {
    // Support multiple TURN server definitions separated by ';' (produced when
    // JSON config used an array). Each element keeps the original
    // "uri,username,password" triple.
    std::vector<std::string> server_entries;
    server_entries.push_back("turn:3.93.50.189:5349?transport=tcp,webrtcsays.ai,wilddolphin");
    for(const auto& entry : server_entries) {
        std::vector<std::string> turnsParams = stringSplit(entry, ",");
        if(turnsParams.size() != 3) {
            RTC_LOG(LS_WARNING) << "TURN entry has " << turnsParams.size() << " segments, expected 3 – skipped: " << entry;
            continue;
        }

        webrtc::PeerConnectionInterface::IceServer iceServer;
        iceServer.uri = turnsParams[0];
        iceServer.username = turnsParams[1];
        iceServer.password = turnsParams[2];

        RTC_LOG(LS_INFO) << "TURN config: uri=" << iceServer.uri
                         << " user='" << iceServer.username
                         << "' pass='" << iceServer.password << "'";
        iceServer.tls_cert_policy = webrtc::PeerConnectionInterface::kTlsCertPolicyInsecureNoCheck;
        config.servers.push_back(iceServer);
    }
  //}

  config.disable_ipv6_on_wifi = true;            // Wi-Fi only
  config.max_ipv6_networks     = 0;   

  // Use default STUN server configuration to avoid complexity

  for (const auto& server : config.servers) {
      if (server.uri.find("stun:") == 0) {
          std::string host_port = server.uri.substr(5);
          size_t colon_pos = host_port.find(':');
          if (colon_pos != std::string::npos) {
              std::string host = host_port.substr(0, colon_pos);
              int port = atoi(host_port.substr(colon_pos + 1).c_str());
              stun_servers.insert(rtc::SocketAddress(host, port));
          }
      } else if (server.uri.find("turn:") == 0 || server.uri.find("turns:") == 0) {
          std::string host_port = server.uri.substr(server.uri.find(":") + 1);
          // Strip off any query parameters
          size_t query_pos = host_port.find('?');
          if (query_pos != std::string::npos) {
              host_port = host_port.substr(0, query_pos);
          }
          size_t colon_pos = host_port.find(':');
          if (colon_pos != std::string::npos) {
              cricket::RelayServerConfig turn_config;
              // Trim username/password just in case
              std::string cred_user = server.username;
              std::string cred_pass = server.password;
              turn_config.credentials = cricket::RelayCredentials(cred_user, cred_pass);
              RTC_LOG(LS_INFO) << "TURN relay credentials user='" << cred_user << "'";

              // Determine desired protocol: default UDP, but respect "?transport=tcp" or "?transport=udp"
              cricket::ProtocolType proto = cricket::PROTO_UDP;

              // 1. Secure URI scheme "turns:" always implies TLS/SSL over TCP
              if (server.uri.find("turns:") == 0) {
                  proto = cricket::PROTO_SSLTCP;

              // 2. Explicit transport parameter overrides plain "turn:" scheme
              } else if (server.uri.find("transport=tcp") != std::string::npos) {
                  proto = cricket::PROTO_TCP;
              } else if (server.uri.find("transport=ssl") != std::string::npos ||
                         server.uri.find("transport=tls") != std::string::npos) {
                  proto = cricket::PROTO_SSLTCP;
              }

              turn_config.tls_cert_policy = cricket::TlsCertPolicy::TLS_CERT_POLICY_INSECURE_NO_CHECK;

              turn_config.ports.push_back(cricket::ProtocolAddress(
                  rtc::SocketAddress(
                      host_port.substr(0, colon_pos),
                      atoi(host_port.substr(colon_pos + 1).c_str())),
                  proto));
              turn_servers.push_back(turn_config);
              RTC_LOG(LS_INFO) << "TURN server parsed: " << host_port << " proto=" << proto << " for URI: " << server.uri;
          } else {
              RTC_LOG(LS_WARNING) << "Failed to parse TURN server port from URI: " << server.uri;
          }
      }
  }

  RTC_LOG(LS_INFO) << "Configured STUN/TURN servers:";
  for (const auto& addr : stun_servers) {
      RTC_LOG(LS_INFO) << "  STUN Server: " << addr.ToString();
  }
  for (const auto& turn : turn_servers) {
      for (const auto& addr : turn.ports) {
          RTC_LOG(LS_INFO) << "  TURN Server: " << addr.address.ToString()
                            << " (Protocol: " << addr.proto << ")";
      }
  }

  // Ensure packet socket factory exists (used for port allocator later)
  if (!socket_factory_) {
    socket_factory_ = std::make_unique<rtc::BasicPacketSocketFactory>(pss());
  }
  // Recreate network manager if needed, passing the PhysicalSocketServer (which is a SocketFactory)
  if (!network_manager_) {
    // Pass pss() which returns the PhysicalSocketServer*, a valid SocketFactory*
    network_manager_ = new rtc::BasicNetworkManager(pss());

    network_manager_->set_network_ignore_list({"docker0", "br0", "veth", "lo"});
  }

  // Add VPN list to ignore
  if(vpns_.size()) {
    for(auto vpn : vpns_) {
      std::string vpn_ip = vpn;
      rtc::NetworkMask vpn_mask(IPFromString(vpn_ip), 32);
      network_manager_->set_vpn_list({vpn_mask});
      network_manager_->set_network_ignore_list({vpn_ip});
      network_manager_->StartUpdating();
    }
  }

  auto port_allocator = std::make_unique<cricket::BasicPortAllocator>(
      network_manager_, socket_factory_.get());
  RTC_DCHECK(port_allocator.get());    

  port_allocator->SetConfiguration(
      stun_servers,
      turn_servers,
      0,  // Keep this as 0
      webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY,
      nullptr,
      {}
  );

  // Allow flexible port allocation for UDP
  uint32_t flags = 0;
  flags |= cricket::PORTALLOCATOR_ENABLE_ANY_ADDRESS_PORTS; 

  port_allocator->set_flags(flags);
  port_allocator->set_step_delay(cricket::kMinimumStepDelay);  // Speed up gathering
  // Candidate filter is derived from config.type (set above to kNoHost or
  // kRelay); no explicit override needed.

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  pc_dependencies.allocator = std::move(port_allocator);

  auto pcf_result = peer_connection_factory_->CreatePeerConnectionOrError(
      config, std::move(pc_dependencies));
  RTC_DCHECK(pcf_result.ok());    
  peer_connection_ = pcf_result.MoveValue();
  RTC_LOG(LS_INFO) << "PeerConnection created successfully.";

  // Explicitly attempt to add video track after peer connection creation if source is available
  if (video_source_) {
    if (rtc::Thread::Current() == signaling_thread()) {
      RTC_LOG(LS_INFO) << "Peer connection created, attempting to add video track immediately.";
      AddVideoTrackIfSourceAvailable();
      RTC_LOG(LS_INFO) << "Post-creation video source state: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
      if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
        RTC_LOG(LS_INFO) << "Video source is live post-peer connection creation, frames should be available.";
      } else {
        RTC_LOG(LS_WARNING) << "Video source is NOT live post-peer connection creation, video may freeze or show black frames.";
      }
    } else {
      signaling_thread()->PostTask([this]() {
        RTC_LOG(LS_INFO) << "Peer connection created, attempting to add video track immediately.";
        AddVideoTrackIfSourceAvailable();
        RTC_LOG(LS_INFO) << "Post-creation video source state: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
        if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
          RTC_LOG(LS_INFO) << "Video source is live post-peer connection creation, frames should be available.";
        } else {
          RTC_LOG(LS_WARNING) << "Video source is NOT live post-peer connection creation, video may freeze or show black frames.";
        }
      });
    }
  } else {
    RTC_LOG(LS_WARNING) << "No video source available to add as a track after peer connection creation. Video will not be sent.";
  }
  // Additional check to ensure video send is active
  EnsureVideoSendActive();
  RTC_LOG(LS_INFO) << "Video send check performed after peer connection creation.";

  return true;
}

void DirectApplication::HandleMessage(rtc::AsyncPacketSocket* socket,
                                      const std::string& message,
                                      const rtc::SocketAddress& remote_addr) {
  RTC_LOG(LS_INFO) << "DirectApplication::HandleMessage: " << message;
  if (message.find("ICE:") == 0) {
    ice_candidates_received_++;
    SendMessage("ICE_ACK:" + std::to_string(ice_candidates_received_));

    // Send our ICE candidate if we haven't sent all
    if (ice_candidates_sent_ < kMaxIceCandidates) {
      ice_candidates_sent_++;
      SendMessage("ICE:" + std::to_string(ice_candidates_sent_));
    }
    // Start SDP exchange when ICE is complete
    else if (ice_candidates_received_ >= kMaxIceCandidates &&
             sdp_fragments_sent_ == 0) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
  } else if (message.find("SDP:") == 0) {
    sdp_fragments_received_++;
    SendMessage("SDP_ACK:" + std::to_string(sdp_fragments_received_));

    // Send our SDP fragment if we haven't sent all
    if (sdp_fragments_sent_ < kMaxSdpFragments) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
    // Send BYE when all exchanges are complete
    else if (sdp_fragments_received_ >= kMaxSdpFragments &&
             ice_candidates_received_ >= kMaxIceCandidates) {
      SendMessage(Msg::kBye);
    }
  } 
  #if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)  
  else if (message.find("LLAMA:") == 0) {
    std::string payload = message.substr(6);
    std::string language, text;
    std::regex re("^\\[([^\\]]+)\\](.*)$");
    std::smatch match;
    if (std::regex_match(payload, match, re)) {
      language = match[1].str();
      text = match[2].str();
    } else {
      language = "en";
      text = payload;
    }
    RTC_LOG(LS_INFO) << "Speaking: [" << language << "] " << text;
    webrtc::SpeechAudioDeviceFactory::NotifyText(text, language);
  } else if (message.find("WHISPER:") == 0) {
    std::string payload = message.substr(8);
    std::string language, text;
    std::regex re("^\\[([^\\]]+)\\](.*)$");
    std::smatch match;
    if (std::regex_match(payload, match, re)) {
      language = match[1].str();
      text = match[2].str();
    } else {
      language = "en";
      text = payload;
    }
    RTC_LOG(LS_INFO) << "Whispering: [" << language << "] " << text;
    webrtc::SpeechAudioDeviceFactory::AskLlama(text, language);
  } 
  #endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
  else if (message == StatusCodes::kOk) {
    ShutdownInternal();            // close PeerConnection

    if (tcp_socket_) {             // <-- NEW
        tcp_socket_->Close();      // close FD
        tcp_socket_.reset();       // **remove from dispatcher**
    }
  }
}

bool DirectApplication::SendMessage(const std::string& message) {
  if (!tcp_socket_) {
    RTC_LOG(LS_ERROR) << "Cannot send message, socket is null";
    return false;
  }
  RTC_LOG(LS_INFO) << "Sending message: " << message;
  size_t sent = tcp_socket_->Send(message.c_str(), message.length(),
                                  rtc::PacketOptions());
  if (sent <= 0) {
    RTC_LOG(LS_ERROR) << "Failed to send message, error: " << tcp_socket_->GetError();
    return false;
  }
  RTC_LOG(LS_INFO) << "Successfully sent " << sent << " bytes";
  return true;
}

rtc::Socket* DirectApplication::WrapSocket(int s) {
  rtc::Socket* socket = DirectApplication::pss()->WrapSocket(s);
  if (socket) {
    tracked_sockets_.push_back(socket);
  }
  return socket;
}

rtc::Socket* DirectApplication::CreateSocket(int family, int type) {
  rtc::Socket* socket = DirectApplication::pss()->CreateSocket(family, type);
  if (socket) {
    tracked_sockets_.push_back(socket);
  }
  return socket;
}

void DirectApplication::OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                           const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    RTC_LOG(LS_INFO) << "Track added: " << receiver->track()->id() << " Kind: " << receiver->track()->kind();

    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        RTC_LOG(LS_INFO) << "Video track added for " << (is_caller() ? "caller" : "callee");
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(receiver->track().get());
        
#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
        // Always create video sink for remote video tracks to prevent freezing
        if(!video_sink_) {
          RTC_LOG(LS_INFO) << "Initializing video sink for remote video track...";
          video_sink_ = std::make_unique<webrtc::LlamaVideoRenderer>();
          ((webrtc::LlamaVideoRenderer*)video_sink_.get())->set_is_llama(opts_.llama);
        }
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
        
        if (video_sink_) {
            RTC_LOG(LS_INFO) << "Attaching video sink to track: " << receiver->track()->id();
            video_track->AddOrUpdateSink(video_sink_.get(), rtc::VideoSinkWants());
            
            // Store reference to remote video track to prevent disconnection
            remote_video_track_ = video_track;
            
            RTC_LOG(LS_INFO) << "Video sink attached successfully";
            // --- Echo video back to remote peer ---
            if (!video_source_) {
                RTC_LOG(LS_INFO) << "Creating EchoVideoTrackSource and outgoing track to echo remote video.";
                auto echo_source = rtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                // Register echo_source as sink to remote video track
                video_track->AddOrUpdateSink(echo_source.get(), rtc::VideoSinkWants());

                // Set as our local video source and add outgoing track
                SetVideoSource(echo_source);
            } else {
                // To avoid casts and RTTI, always create a dedicated EchoVideoTrackSource
                // for echoing the remote video back, without touching the existing
                // local video_source_ (which may be camera etc.).
                auto echo_source = rtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                video_track->AddOrUpdateSink(echo_source.get(), rtc::VideoSinkWants());
                // Optionally store echo_source if you want to keep it alive explicitly,
                // but since AddOrUpdateSink doesn't retain it, we need to hold the ref.
                echo_sources_.push_back(echo_source);  // Add vector<scoped_refptr<EchoVideoTrackSource>> echo_sources_ to class
            }
            RTC_LOG(LS_INFO) << "Receiver: Video track state: " << video_track->state();
            RTC_LOG(LS_INFO) << "Receiver: Video track enabled: " << (video_track->enabled() ? "true" : "false");
        } else {
            RTC_LOG(LS_ERROR) << "Video sink is still nullptr, cannot attach to track: " << receiver->track()->id();
        }
    } else if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        RTC_LOG(LS_INFO) << "Audio track added.";
        // Handle audio track if needed
    }
}

void DirectApplication::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    RTC_LOG(LS_INFO) << "Track removed: " << receiver->track()->id();
    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        if (video_sink_ && remote_video_track_) {
            RTC_LOG(LS_INFO) << "Removing video sink from remote track for " << (is_caller() ? "caller" : "callee");
            remote_video_track_->RemoveSink(video_sink_.get());
        }
        // Clear remote video track reference
        remote_video_track_ = nullptr;
        RTC_LOG(LS_INFO) << "Remote video track reference cleared";
    }
}

// Store an external video source (injected by host) for use when creating video tracks.
bool DirectApplication::SetVideoSource(
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source) {
  if (signaling_thread() && rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this, video_source]() {
        // Safeguard: Prevent overwriting a live source with null during active connection
        if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
            !video_source && peer_connection_ && 
            peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
            RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
            return;
        }
        video_source_ = video_source;
        AddVideoTrackIfSourceAvailable();
    });
    return true; // deferred
  }

  // Safeguard: Prevent overwriting a live source with null during active connection
  if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
      !video_source && peer_connection_ && 
      peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
    RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
    return true;
  }
  video_source_ = video_source;
  AddVideoTrackIfSourceAvailable();
  RTC_LOG(LS_INFO) << "Video source set for " << (is_caller() ? "caller" : "callee");
  return true;
}

bool DirectApplication::SetVideoSink(
    std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink) {
  // Ensure assignment happens on the signaling thread for safety.
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->BlockingCall([this, &video_sink]() {
      video_sink_ = std::move(video_sink);
    });
  } else {
      video_sink_ = std::move(video_sink);
  }

  RTC_LOG(LS_INFO) << "Video sink set for " << (is_caller() ? "caller" : "callee");
  return true;
}

// Method to add video track if source is available and peer connection exists
void DirectApplication::AddVideoTrackIfSourceAvailable() {
  if (!video_source_ || !peer_connection_) return;

  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this]() { AddVideoTrackIfSourceAvailable(); });
    return;
  }

  RTC_LOG(LS_INFO) << "Adding video track to existing peer connection for " << (is_caller() ? "caller" : "callee");
  RTC_LOG(LS_INFO) << "Video source state before adding track: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
  rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
      peer_connection_factory_->CreateVideoTrack(video_source_, "video_track"));
  video_track_ = video_track;
  video_track_->set_enabled(true);
  auto result = peer_connection_->AddTrack(video_track, {"stream1"});
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: " << result.error().message();
  } else {
    RTC_LOG(LS_INFO) << "Video track added successfully to PeerConnection";
    RTC_LOG(LS_INFO) << "Sender: Video track enabled: " << (video_track_->enabled() ? "true" : "false");
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive && video_track_->enabled()) {
      RTC_LOG(LS_INFO) << "Sender: Video source is live and track is enabled. Frames should be sent to remote peer.";
    } else {
      RTC_LOG(LS_WARNING) << "Sender: Video source or track state issue. Black frames may be sent to remote peer.";
    }
    // Safeguard: Ensure video send is active with the current source
    EnsureVideoSendActive();
  }
}

// New method to ensure video send is active with the current source
void DirectApplication::EnsureVideoSendActive() {
  if (video_track_ && video_source_ && peer_connection_) {
    RTC_LOG(LS_INFO) << "Ensuring video send is active for SSRC associated with track.";
    
    // Check if video source is live and track is enabled
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_INFO) << "Video source is live, ensuring it remains active for sending.";
      
      // Ensure video track is enabled
      if (!video_track_->enabled()) {
        RTC_LOG(LS_WARNING) << "Video track was disabled, re-enabling to prevent freezing.";
        video_track_->set_enabled(true);
      }
      
      // Log current connection state for debugging
      if (peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
        RTC_LOG(LS_INFO) << "ICE connected - video should be flowing normally.";
      }
      
    } else {
      RTC_LOG(LS_WARNING) << "Video source is not live, cannot ensure active video send.";
    }
  } else {
    RTC_LOG(LS_WARNING) << "Cannot ensure video send active: missing track (" << (video_track_ ? "present" : "missing") 
                        << "), source (" << (video_source_ ? "present" : "missing") 
                        << "), or peer connection (" << (peer_connection_ ? "present" : "missing") << ").";
  }
}
