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

#ifndef AUDIO_DEVICE_WHISPER_AUDIO_DEVICE_H_
#define AUDIO_DEVICE_WHISPER_AUDIO_DEVICE_H_

#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "rtc_base/platform_thread.h"
#include "rtc_base/system/file_wrapper.h"
#include "rtc_base/time_utils.h"

#include "speech_audio_device.h"
#include "whillats.h" // whillats project - Whisper, Llama, Espeak TTS

namespace webrtc {

class WhisperAudioDevice : public SpeechAudioDevice {
 public:
  // Constructor taking input filename and output log filename
  WhisperAudioDevice(TaskQueueFactory* task_queue_factory);
  virtual ~WhisperAudioDevice();

  // Implement all pure virtual methods from AudioDeviceGeneric

  // Active audio layer
  int32_t ActiveAudioLayer(AudioDeviceModule::AudioLayer& audioLayer) const override;

  // Initialization
  InitStatus Init() override;
  int32_t Terminate() override;
  bool Initialized() const override;

  // Send text to speaking queue
  virtual void speakText(const std::string& text) override;
  // Send question to llama
  virtual void askLlama(const std::string& text) override;

  // Device enumeration
  int16_t PlayoutDevices() override;
  int16_t RecordingDevices() override;
  int32_t PlayoutDeviceName(uint16_t index, 
                            char name[kAdmMaxDeviceNameSize],
                            char guid[kAdmMaxGuidSize]) override;
  int32_t RecordingDeviceName(uint16_t index, 
                              char name[kAdmMaxDeviceNameSize],
                              char guid[kAdmMaxGuidSize]) override;

  // Device selection
  int32_t SetPlayoutDevice(uint16_t index) override;
  int32_t SetPlayoutDevice(AudioDeviceModule::WindowsDeviceType device) override;
  int32_t SetRecordingDevice(uint16_t index) override;
  int32_t SetRecordingDevice(AudioDeviceModule::WindowsDeviceType device) override;

  // Audio transport initialization
  int32_t PlayoutIsAvailable(bool& available) override;
  int32_t InitPlayout() override;
  bool PlayoutIsInitialized() const override;
  int32_t RecordingIsAvailable(bool& available) override;
  int32_t InitRecording() override;
  bool RecordingIsInitialized() const override;

  // Audio transport control
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t StartRecording() override;
  int32_t StopRecording() override;
  bool Recording() const override;

  // Other required methods (volume, mute, etc.)
  int32_t InitSpeaker() override;
  bool SpeakerIsInitialized() const override;
  int32_t InitMicrophone() override;
  bool MicrophoneIsInitialized() const override;

  // Implement other required methods from AudioDeviceGeneric
  // (volume controls, mute controls, stereo support, etc.)
  // Speaker volume controls
  int32_t SpeakerVolumeIsAvailable(bool& available) override;
  int32_t SetSpeakerVolume(uint32_t volume) override;
  int32_t SpeakerVolume(uint32_t& volume) const override;
  int32_t MaxSpeakerVolume(uint32_t& maxVolume) const override;
  int32_t MinSpeakerVolume(uint32_t& minVolume) const override;

  // Microphone volume controls
  int32_t MicrophoneVolumeIsAvailable(bool& available) override;
  int32_t SetMicrophoneVolume(uint32_t volume) override;
  int32_t MicrophoneVolume(uint32_t& volume) const override;
  int32_t MaxMicrophoneVolume(uint32_t& maxVolume) const override;
  int32_t MinMicrophoneVolume(uint32_t& minVolume) const override;

  // Speaker mute control
  int32_t SpeakerMuteIsAvailable(bool& available) override;
  int32_t SetSpeakerMute(bool enable) override;
  int32_t SpeakerMute(bool& enabled) const override;

  // Microphone mute control
  int32_t MicrophoneMuteIsAvailable(bool& available) override;
  int32_t SetMicrophoneMute(bool enable) override;
  int32_t MicrophoneMute(bool& enabled) const override;

  // Stereo support
  int32_t StereoPlayoutIsAvailable(bool& available) override;
  int32_t SetStereoPlayout(bool enable) override;
  int32_t StereoPlayout(bool& enabled) const override;
  int32_t StereoRecordingIsAvailable(bool& available) override;
  int32_t SetStereoRecording(bool enable) override;
  int32_t StereoRecording(bool& enabled) const override;

  // Delay information and control
  int32_t PlayoutDelay(uint16_t& delayMS) const override;

  void AttachAudioBuffer(AudioDeviceBuffer* audioBuffer) override;

  void OnDataReady(const std::vector<short>& audioData);

  void SetTTSBuffer(const uint16_t* buffer, size_t buffer_size);

 private:
  bool RecThreadProcess();
  bool PlayThreadProcess();

  webrtc::TaskQueueFactory* _task_queue_factory;

   // Similar members to FileAudioDevice
  AudioDeviceBuffer* _ptrAudioBuffer;
  int8_t* _recordingBuffer;
  int8_t* _playoutBuffer;

  Mutex mutex_;
  rtc::PlatformThread _ptrThreadRec;
  rtc::PlatformThread _ptrThreadPlay;

  size_t _recordingFramesIn10MS;
  size_t _playoutFramesIn10MS;

  uint32_t _recordingFramesLeft;
  uint32_t _playoutFramesLeft;

  bool _recording;
  bool _playing;
  
  int64_t _lastCallPlayoutMillis;
  int64_t _lastCallRecordMillis;

  FileWrapper _recFile;
  FileWrapper _playFile;
  
  WhillatsSetAudioCallback _ttsCallback;
  WhillatsSetResponseCallback _whisperCallback;
  WhillatsSetLanguageCallback _languageCallback;
  WhillatsSetResponseCallback _llamaResponseCallback;
  
  // Protects concurrent access to _whisper_transcriber across the playout
  // thread and control thread (StartPlayout / StopPlayout).
  mutable std::mutex _transcriber_mutex;
  WhillatsTranscriber* _whisper_transcriber = nullptr; // guarded by _transcriber_mutex
  WhillatsTTS* _tts = nullptr;
  WhillatsLlama* _llama_device = nullptr;

  std::queue<std::string> _textQueue;
  absl::Mutex _queueMutex;
  std::condition_variable _queueCondition;
  
  std::vector<uint16_t> _ttsBuffer;  // Instance member to hold TTS audio
  size_t _ttsIndex = 0;  // Instance member to track buffer index

  std::mutex audio_buffer_mutex;
  std::condition_variable buffer_cv;
};

}  // namespace webrtc

#endif  // AUDIO_DEVICE_WHISPER_AUDIO_DEVICE_H_
