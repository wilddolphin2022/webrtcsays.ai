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

#pragma once

#include "modules/audio_device/audio_device_generic.h"

class SpeechAudioDevice : public webrtc::AudioDeviceGeneric {
 public:

  virtual void speakText(const std::string& text) = 0;
  virtual void askLlama(const std::string& text) = 0;

  bool _whisper_enabled = false;
  bool _llama_enabled = false;
  bool _tts_enabled = false;

  virtual ~SpeechAudioDevice() {}
};

