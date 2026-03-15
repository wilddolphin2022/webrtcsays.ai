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

#include "modules/third_party/whillats/src/whillats.h"

// Stub implementations of forward-declared classes
class ESpeakTTS {
public:
    ESpeakTTS(WhillatsSetAudioCallback) {}
    ~ESpeakTTS() {}
};

class WhisperTranscriber {
public:
    WhisperTranscriber() {}
    ~WhisperTranscriber() {}
};

class LlamaDeviceBase {
public:
    LlamaDeviceBase() {}
    ~LlamaDeviceBase() {}
};

WhillatsTTS::WhillatsTTS(WhillatsSetAudioCallback callback) 
    : _callback(callback), _espeak_tts(nullptr) {}

WhillatsTTS::~WhillatsTTS() {}

bool WhillatsTTS::start() { return false; }
void WhillatsTTS::stop() {}
void WhillatsTTS::queueText(const char*) {}
int WhillatsTTS::getSampleRate() { return 16000; }

WhillatsTranscriber::WhillatsTranscriber(const char*, WhillatsSetResponseCallback callback) 
    : _callback(callback), _whisper_transcriber(nullptr) {}

WhillatsTranscriber::~WhillatsTranscriber() {}

bool WhillatsTranscriber::start() { return false; }
void WhillatsTranscriber::stop() {}
void WhillatsTranscriber::processAudioBuffer(uint8_t*, const size_t) {}

WhillatsLlama::WhillatsLlama(const char*, WhillatsSetResponseCallback callback)
    : _callback(callback), _llama_device(nullptr) {}

WhillatsLlama::~WhillatsLlama() {}

bool WhillatsLlama::start() { return false; }
void WhillatsLlama::stop() {}
void WhillatsLlama::askLlama(const char*) {}