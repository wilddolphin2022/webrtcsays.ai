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


#include <stdio.h>
#include <cstdlib>

#include "absl/strings/string_view.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

#include "api/task_queue/default_task_queue_factory.h"
#include "modules/audio_device/speech/speech_audio_device_factory.h"
#include "modules/audio_device/speech/whisper_audio_device.h"
#include "modules/third_party/whillats/src/whillats_utils.h"
#include "absl/synchronization/mutex.h"

namespace webrtc {

std::unique_ptr<TaskQueueFactory> SpeechAudioDeviceFactory::_taskQueueFactory;

std::unique_ptr<WhillatsTranscriber> SpeechAudioDeviceFactory::_whisperDevice;
std::unique_ptr<WhillatsLlama> SpeechAudioDeviceFactory::_llamaDevice;
std::unique_ptr<WhillatsTTS> SpeechAudioDeviceFactory::_ttsDevice;

std::string SpeechAudioDeviceFactory::_whisperModelFilename;
std::string SpeechAudioDeviceFactory::_llamaModelFilename;
std::string SpeechAudioDeviceFactory::_llavaMMProjFilename;
std::string SpeechAudioDeviceFactory::_wavFilename;
std::string SpeechAudioDeviceFactory::_yuvFilename;
YUVData SpeechAudioDeviceFactory::_yuvData;
bool SpeechAudioDeviceFactory::_whisperEnabled = false;
bool SpeechAudioDeviceFactory::_llamaEnabled = false;

// Static queue and mutex for enqueued TTS text tasks
std::unique_ptr<TaskQueueBase, TaskQueueDeleter> SpeechAudioDeviceFactory::_textToSpeakQueue;
absl::Mutex SpeechAudioDeviceFactory::_textToSpeakQueueMutex;

WhillatsTTS* SpeechAudioDeviceFactory::CreateWhillatsTTS(WhillatsSetAudioCallback &ttsCallback) {

  if(_ttsDevice)
    return _ttsDevice.get();

  _ttsDevice = std::make_unique<WhillatsTTS>(ttsCallback);
  return _ttsDevice.get();
}

WhillatsTranscriber* SpeechAudioDeviceFactory::CreateWhillatsTranscriber
  (WhillatsSetResponseCallback &whisperCallback, WhillatsSetLanguageCallback &languageCallback) {
  if(_whisperEnabled) {
    if(_whisperDevice)
      return _whisperDevice.get();

    _whisperDevice = std::make_unique<WhillatsTranscriber>(_whisperModelFilename.c_str(), whisperCallback, languageCallback);
    return _whisperDevice.get();
  }
  return nullptr;
}

WhillatsLlama* SpeechAudioDeviceFactory::CreateWhillatsLlama(WhillatsSetResponseCallback &llamaCallback) {
  if(_llamaEnabled) {
    if(_llamaDevice)
      return _llamaDevice.get();

    _llamaDevice = std::make_unique<WhillatsLlama>(_llamaModelFilename.c_str(), _llavaMMProjFilename.c_str(), llamaCallback);
    return _llamaDevice.get();
  }
  return nullptr;
}

void SpeechAudioDeviceFactory::NotifyText(const std::string& text, const std::string& language) { 
  if(!_ttsDevice) {
    static AudioCallback ttsCallback = [](bool success, const uint16_t* buffer, size_t buffer_size, void* user_data) {};
    static  WhillatsSetAudioCallback callback(ttsCallback, nullptr);
    WhillatsTTS* tts = CreateWhillatsTTS(callback);
    if(tts) {
      // Starting TTS via notifications, avoiding audio processor. Valuable for iOS.
      tts->start(false);
    }
  }
  if(_ttsDevice)
    _ttsDevice->queueText(text.c_str(), language.c_str());
}

void SpeechAudioDeviceFactory::SpeakText(const std::string& text, const std::string& language) { 
  if(_ttsDevice)
    _ttsDevice->queueText(text.c_str(), language.c_str());
}

void SpeechAudioDeviceFactory::AskLlama(const std::string& text, const std::string& language) { 
  if(_llamaDevice)
    _llamaDevice->askLlama(text.c_str());
}

void SpeechAudioDeviceFactory::SetWhisperModelFilename(absl::string_view whisper_model_filename) {
  _whisperModelFilename = whisper_model_filename;
}

void SpeechAudioDeviceFactory::SetLlamaModelFilename(absl::string_view llama_model_filename) {
  _llamaModelFilename = llama_model_filename;
}

void SpeechAudioDeviceFactory::SetLlavaMMProjFilename(absl::string_view llava_mmproj_filename) {
  _llavaMMProjFilename = llava_mmproj_filename;
}

void SpeechAudioDeviceFactory::SetWavFilename(absl::string_view wav_filename) {
  _wavFilename = wav_filename;
}

void SpeechAudioDeviceFactory::SetYuvFilename(absl::string_view yuv_filename, int width, int height) {
  _yuvFilename = yuv_filename;
  load_yuv(_yuvData, _yuvFilename.c_str(), width, height);
}

void SpeechAudioDeviceFactory::SetTaskQueueFactory(TaskQueueFactory* task_queue_factory) {
  _taskQueueFactory.reset(task_queue_factory);
}

void SpeechAudioDeviceFactory::ResetDevices() {
  // Ensure any long-running processing threads are stopped before deleting.
  if (_ttsDevice) {
    _ttsDevice->stop();
    _ttsDevice.reset();
  }
  if (_whisperDevice) {
    _whisperDevice->stop();
    _whisperDevice.reset();
  }
  if (_llamaDevice) {
    _llamaDevice->stop();
    _llamaDevice.reset();
  }
}

AudioDeviceGeneric* SpeechAudioDeviceFactory::CreateSpeechAudioDevice() {
  WhisperAudioDevice* whisper_audio_device = nullptr;
  if(!whisper_audio_device) {
    whisper_audio_device = new WhisperAudioDevice(_taskQueueFactory.get());
    RTC_LOG(LS_INFO) << "Initialized WhisperAudioDevice instance.";
  }

  return static_cast<AudioDeviceGeneric*>(whisper_audio_device);
}

}  // namespace webrtc
