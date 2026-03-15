/*
 *  (c) 2025, wilddolphin2022 
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2022/webrtcsays.ai
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef AUDIO_DEVICE_SPEECH_AUDIO_DEVICE_FACTORY_H_
#define AUDIO_DEVICE_SPEECH_AUDIO_DEVICE_FACTORY_H_

#include <stdint.h>
#include <memory>
#include <queue>
#include "absl/synchronization/mutex.h"

#include "absl/strings/string_view.h"
#include "api/task_queue/task_queue_factory.h"
#include "modules/audio_device/audio_device_generic.h"

#include "modules/third_party/whillats/src/whillats.h"

namespace webrtc {

// This class is used by audio_device_impl.cc when WebRTC is compiled with
// WEBRTC_SPEECH_DEVICES. The application must include this file and set the
// filenames to use before the audio device module is initialized. This is
// intended for test tools which use the audio device module.
class SpeechAudioDeviceFactory {
 public:
  static AudioDeviceGeneric* CreateSpeechAudioDevice();
  static void SetTaskQueueFactory(TaskQueueFactory* task_queue_factory);

  // Release cached helper devices so a new AudioDevice instance can create
  // fresh ones with the correct callback/user_data pointers.
  static void ResetDevices();

  static void SetWhisperModelFilename(absl::string_view whisper_model_filename);
  static void SetLlamaModelFilename(absl::string_view llama_model_filename);
  static void SetLlavaMMProjFilename(absl::string_view llava_mmproj_filename);
  static void SetWavFilename(absl::string_view wav_filename);
  static void SetYuvFilename(absl::string_view yuv_filename, int width, int height);
  static const std::string& GetWhisperModelFilename() { return _whisperModelFilename; }
  static const std::string& GetLlamaModelFilename() { return _llamaModelFilename; }
  static const std::string& GetLlavaMMProjFilename() { return _llavaMMProjFilename; }
  static const std::string& GetWavFilename() { return _wavFilename; }
  static const std::string& GetYuvFilename() { return _yuvFilename; }
  static YUVData& GetYuvData() { return _yuvData; }
  static std::string GetLanguage() { 
    return _whisperDevice ? _whisperDevice->getLanguage() : "en"; }
  static void NotifyText(const std::string& text, const std::string& language);
  static void SpeakText(const std::string& text, const std::string& language);
  static void AskLlama(const std::string& text, const std::string& language);

  static void SetWhisperEnabled(bool enabled) { _whisperEnabled = enabled; }
  static void SetLlamaEnabled(bool enabled) { _llamaEnabled = enabled; }
  static void SetLanguage(const std::string& language) { 
    if(_whisperDevice) _whisperDevice->setLanguage(language.c_str()); }

  static WhillatsTTS* tts() { return _ttsDevice.get(); }
  static WhillatsTranscriber* whisper() { return _whisperDevice.get(); }
  static WhillatsLlama* llama() { return _llamaDevice.get(); }

  static WhillatsTTS* CreateWhillatsTTS(
    WhillatsSetAudioCallback &ttsCallback);
  static WhillatsTranscriber* CreateWhillatsTranscriber(
    WhillatsSetResponseCallback &whisperCallback,
    WhillatsSetLanguageCallback &languageCallback);
  static WhillatsLlama* CreateWhillatsLlama(
    WhillatsSetResponseCallback &llamaCallback);

  friend class WhisperAudioDevice;
 private:
  static std::unique_ptr<TaskQueueFactory> _taskQueueFactory;

  enum : uint32_t { MAX_FILENAME_LEN = 512 };

  // The input whisper model file must be a ggml file (https://github.com/ggerganov/whisper.cpp/blob/master/models/README.md)
  static std::string _whisperModelFilename;
  // The input llama model file must be a gguf file
  static std::string _llamaModelFilename;
  // The input llava mmproj file must be a gguf file
  static std::string _llavaMMProjFilename;
  // This is a wav file, 16k samples, 16 bit PCM, to play out on beginning
  static std::string _wavFilename;
  // This is a yuv file, to send to llama-llava
  static std::string _yuvFilename;
  // This is a static yuv data, to send to llama-llava
  static YUVData _yuvData;
  
  static bool _whisperEnabled;
  static bool _llamaEnabled;

  // This is a whisper device, to send to whisper
  static std::unique_ptr<WhillatsTranscriber> _whisperDevice;
  // This is a llama device, to send to llama-llava
  static std::unique_ptr<WhillatsLlama> _llamaDevice;
  // This is a tts device, to send to tts
  static std::unique_ptr<WhillatsTTS> _ttsDevice;

  // Queue of text to speak
  static std::unique_ptr<TaskQueueBase, TaskQueueDeleter> _textToSpeakQueue;
  static absl::Mutex _textToSpeakQueueMutex;
  // Enqueue text with specified language for TTS
  static void addTextToSpeakQueue(const std::string& text, const std::string& language);
};

}  // namespace webrtc

#endif  // AUDIO_DEVICE_SPEECH_AUDIO_DEVICE_FACTORY_H_
