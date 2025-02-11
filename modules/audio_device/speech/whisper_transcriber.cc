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

#include <iostream>
#include <regex>

#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include <whisper.h>
#include "whisper_transcriber.h"
#include "silence_finder.h"  // Silence finder code

WhisperTranscriber::WhisperTranscriber(
    SpeechAudioDevice* speech_audio_device,
      webrtc::TaskQueueFactory* task_queue_factory, 
      const std::string& inputFilename) 
    : _speech_audio_device(speech_audio_device),
      _task_queue_factory(task_queue_factory),
      _whisperContext(nullptr),
      _audioBuffer(kRingBufferSizeIncrement),
      _running(false),
      _processingActive(false),
      _overflowCount(0),
      _ringBufferSize(kRingBufferSizeIncrement)
{
    // Reserve space for audio buffer
    _accumulatedByteBuffer.reserve(kSampleRate * kTargetDurationSeconds * 2); // 16-bit samples
    _modelFilename = inputFilename;

    // Initialize Whisper context
    if (!InitializeWhisperModel(_modelFilename) || !_whisperContext) {
        RTC_LOG(LS_ERROR) << "Failed to initialize Whisper model";
        _whisperContext = TryAlternativeInitMethods(_modelFilename);
        if (!_whisperContext) {
            RTC_LOG(LS_ERROR) << "Failed to initialize Whisper model alternative ways";
        }
    }

    // Creating task pool
    if(!_task_queue_factory)
        _task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();

    RTC_LOG(LS_INFO) << "Creating TaskQueuePool on thread " << rtc::Thread::Current() << ", using factory " << task_queue_factory;
    _task_queue_pool.reset(new TaskQueuePool(_task_queue_factory.get(), 
       std::min(16, static_cast<int>(std::thread::hardware_concurrency()))));
}

WhisperTranscriber::~WhisperTranscriber() {
    Stop();
    if (_whisperContext) {
        whisper_free(_whisperContext);
    }
}

bool WhisperTranscriber::TranscribeAudioNonBlocking(const std::vector<float>& pcmf32) {
    // Prevent multiple simultaneous processing attempts
    if (_processingActive.exchange(true)) {
        RTC_LOG(LS_WARNING) << "Whisper transcription already in progress";
        return false;
    }

    // Validate context
    if (!_whisperContext) {
        RTC_LOG(LS_ERROR) << "Whisper context is null during transcription";
        _processingActive = false;
        return false;
    }

    // Validate input
    if (pcmf32.empty()) {
        RTC_LOG(LS_ERROR) << "Empty audio buffer for transcription";
        _processingActive = false;
        return false;
    }

    // Input size validation
    if (pcmf32.size() < kSampleRate || pcmf32.size() > kTargetSamples) {
        RTC_LOG(LS_ERROR) << "Unexpected audio input size: " << pcmf32.size();
        _processingActive = false;
        return false;
    }

    // Input data validation without exceptions
    bool validInput = true;
    float sum = 0.0f, squaredSum = 0.0f;
    float minVal = pcmf32[0], maxVal = pcmf32[0];

    for (size_t i = 0; i < pcmf32.size(); ++i) {
        float sample = pcmf32[i];
        
        // Check for NaN or infinite values
        if (!(sample == sample) || std::abs(sample) > 1.0f) {
            RTC_LOG(LS_ERROR) << "Invalid sample at index " << i << ": " << sample;
            validInput = false;
            break;
        }

        sum += sample;
        squaredSum += sample * sample;
        minVal = std::min(minVal, sample);
        maxVal = std::max(maxVal, sample);
    }

    if (!validInput) {
        _processingActive = false;
        return false;
    }

    // Compute statistics
    float mean = sum / pcmf32.size();
    float variance = (squaredSum / pcmf32.size()) - (mean * mean);

    RTC_LOG(LS_VERBOSE) << "Audio Input Analysis:"
                    << " Samples=" << pcmf32.size()
                    << " Mean=" << mean
                    << " Variance=" << variance
                    << " Min=" << minVal
                    << " Max=" << maxVal;

    // Prepare Whisper parameters
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    
    wparams.print_realtime = false;
    wparams.print_progress = false;
    wparams.language = "en";
    wparams.translate = false;
    wparams.n_threads = std::min(4, static_cast<int>(std::thread::hardware_concurrency()));
    
    wparams.n_max_text_ctx = 64;
 
    // Diagnostic logging before transcription
    RTC_LOG(LS_INFO) << "Preparing Whisper Transcription:"
                    << " Threads=" << wparams.n_threads
                    << " Max Text Context=" << wparams.n_max_text_ctx;

    if (!_whisperContext) {
        RTC_LOG(LS_ERROR) << "Failed to initialize Whisper model!";
        return false;
    }

    int result = 0;
    // Attempt transcription
    result = whisper_full(
        _whisperContext,
        wparams, 
        pcmf32.data(), 
        pcmf32.size()
        );

    // Process results
    if (result == 0) {
        int numSegments = whisper_full_n_segments(_whisperContext);
        RTC_LOG(LS_VERBOSE) << "Transcription completed. Segments: " << numSegments;

        // Collect and log segments
        std::string fullTranscription;
        for (int i = 0; i < numSegments; ++i) {
            const char* text = whisper_full_get_segment_text(_whisperContext, i);
            if (text && strlen(text) > 0) {
                fullTranscription += std::string(text) + " ";
                RTC_LOG(LS_VERBOSE) << "Segment " << i << ": " << text;
            }
        }

     if (!fullTranscription.empty()) {
            RTC_LOG(LS_VERBOSE) << "Full Transcription: " << fullTranscription;
            // Remove text within brackets and the brackets themselves
            std::string cleanTranscription = std::regex_replace(fullTranscription, 
                std::regex("\\[.*?\\]|\\(.*?\\)|\\{.*?\\}"), "");
            
            if(_speech_audio_device && !cleanTranscription.empty()) {
              if(_speech_audio_device->_llaming)
                _speech_audio_device->askLlama(cleanTranscription);
              else {
                _speech_audio_device->speakText(cleanTranscription);
              }
            }
        }      
      
    } else {
        RTC_LOG(LS_ERROR) << "Whisper transcription failed. Error code: " << result;
    }

    // Reset processing flag
    _processingActive = false;

    return result == 0;
}

bool WhisperTranscriber::RunProcessingThread() {
    std::vector<uint8_t> audioBuffer;
    
    while (_running && _audioBuffer.availableToRead() > 0) {
        // Ensure audioBuffer is sized correctly to receive data
        audioBuffer.resize(_audioBuffer.availableToRead());
        if (_audioBuffer.read(audioBuffer.data(), audioBuffer.size())) {
            // Create a local copy for the lambda
            std::vector<uint8_t> localAudioBuffer = audioBuffer;
            _task_queue_pool->enqueue([this, localAudioBuffer = std::move(localAudioBuffer)]() mutable {
                // Perform Whisper transcription
                if (_whisperContext && localAudioBuffer.size()) {
                    if (localAudioBuffer.size() % 2 != 0) {
                        RTC_LOG(LS_WARNING) << "Audio buffer size is not even: " << localAudioBuffer.size();
                        return /*false*/; // or handle this case appropriately
                    }

                    // Convert PCM16 buffer to float
                    std::vector<float> pcmf32;
                    pcmf32.reserve(localAudioBuffer.size() / 2);

                    for (size_t i = 0; i < localAudioBuffer.size(); i += 2) {
                        // For little-endian PCM16
                        int16_t sample = (int16_t)(localAudioBuffer[i]) | ((int16_t)(localAudioBuffer[i + 1]) << 8);
                        pcmf32.push_back(sample / 32768.0f);
                    }
                    localAudioBuffer.clear();

                    // Add this before transcription
                    RTC_LOG(LS_INFO) << "Audio input details:"
                                    << " First sample: " << pcmf32[0]
                                    << " Last sample: " << pcmf32.back()
                                    << " Sample range: [" 
                                    << *std::min_element(pcmf32.begin(), pcmf32.end()) 
                                    << ", " 
                                    << *std::max_element(pcmf32.begin(), pcmf32.end()) 
                                    << "]";
                    
                    // Use non-blocking transcription
                    TranscribeAudioNonBlocking(pcmf32);
                }
            });

            // Small sleep to prevent tight looping
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }            

        // Sleep if no data available to read to prevent busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } 

    return true;
}

bool WhisperTranscriber::ValidateWhisperModel(const std::string& modelPath) {
    // Check file existence
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        RTC_LOG(LS_ERROR) << "Cannot open model file: " << modelPath;
        return false;
    }

    // Check file size (Whisper models are typically large)
    std::streamsize fileSize = file.tellg();
    file.close();

    // Typical Whisper model sizes range from 100MB to 1.5GB
    const int64_t minModelSize = static_cast<int64_t>(100) * 1024 * 1024;
    const int64_t maxModelSize = static_cast<int64_t>(2) * 1024 * 1024 * 1024;

    if (fileSize < minModelSize || fileSize > maxModelSize) {
        RTC_LOG(LS_ERROR) << "Unexpected model file size: " << fileSize << " bytes";
        return false;
    }

    return true;
}

bool WhisperTranscriber::InitializeWhisperModel(const std::string& modelPath) {
    // Open the file in binary mode
    FILE* file = fopen(modelPath.c_str(), "rb");
    if (!file) {
        RTC_LOG(LS_ERROR) << "Cannot open model file: " << modelPath;
        return false;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Log detailed file information
    RTC_LOG(LS_INFO) << "Model file path: " << modelPath;
    RTC_LOG(LS_INFO) << "Model file size: " << fileSize << " bytes";

    // Read first few bytes to check file signature
    unsigned char header[16];
    size_t bytesRead = fread(header, 1, sizeof(header), file);
    fclose(file);

    if (bytesRead < sizeof(header)) {
        RTC_LOG(LS_ERROR) << "Failed to read model file header";
        return false;
    }

    // Log header bytes for diagnostic purposes
    std::stringstream headerHex;
    headerHex << "Model file header (first 16 bytes): ";
    for (size_t i = 0; i < bytesRead; ++i) {
        headerHex << std::hex << std::setw(2) << std::setfill('0') 
                << static_cast<int>(header[i]) << " ";
    }
    RTC_LOG(LS_INFO) << headerHex.str();

    // Attempt model initialization with verbose error checking
    whisper_context_params context_params = whisper_context_default_params();
    
    // Try different GPU configuration options
    std::vector<bool> gpuOptions = { true };
    
    for (bool useGpu : gpuOptions) {
        context_params.use_gpu = useGpu;
        
        // Detailed logging before model initialization attempt
        RTC_LOG(LS_INFO) << "Attempting to load model with GPU " 
                        << (useGpu ? "Enabled" : "Disabled")
                        ;

        // Try to initialize the model
        whisper_context* localContext = whisper_init_from_file_with_params(
            modelPath.c_str(), 
            context_params
        );

        if (localContext) {
            _whisperContext = localContext;
            RTC_LOG(LS_INFO) << "Model loaded successfully (GPU: " 
                            << (useGpu ? "Enabled" : "Disabled") << ")";
            return true;
        }

        RTC_LOG(LS_WARNING) << "Model load failed with GPU " 
                            << (useGpu ? "Enabled" : "Disabled");
    }

    RTC_LOG(LS_ERROR) << "Failed to load Whisper model from: " << modelPath;
    return false;
}

whisper_context* WhisperTranscriber::TryAlternativeInitMethods(const std::string& modelPath) {
    // Method 1: Direct file initialization
    whisper_context* ctx = nullptr;

    // Method 2: Low-level initialization
    whisper_context_params params = whisper_context_default_params();
    params.use_gpu = false;
    
    // If your Whisper.cpp version supports this
    ctx = whisper_init_from_file_with_params(modelPath.c_str(), params);
    
    return ctx;
}

void WhisperTranscriber::ProcessAudioBuffer(uint8_t* playoutBuffer, size_t kPlayoutBufferSize) {
    // Convert from uint8_t to int16_t for processing
    std::vector<int16_t> int16Buffer;
    int16Buffer.reserve(kPlayoutBufferSize / 2);
    for (size_t i = 0; i < kPlayoutBufferSize; i += 2) {
        int16_t sample = (int16_t)(playoutBuffer[i]) | ((int16_t)(playoutBuffer[i + 1]) << 8);
        int16Buffer.push_back(sample);
    }

    // More conservative threshold and larger window
    const float relativeThreshold = 0.1f;  // Increased to 10%
    const uint windowSize = kSampleRate / 4;  // 250ms window
    
    // Add hysteresis to prevent rapid switching
    const float voiceStartThreshold = relativeThreshold * 1.2f;  // Higher threshold to start voice
    const float voiceEndThreshold = relativeThreshold * 0.8f;   // Lower threshold to end voice

    SilenceFinder<int16_t> silenceFinder(int16Buffer.data(), int16Buffer.size(), kSampleRate);
    auto silenceRegions = silenceFinder.find(relativeThreshold, windowSize);
    
    bool voicePresent = false;
    if (silenceFinder.avgAmplitude == 0) {
        //RTC_LOG(LS_ERROR) << "Average amplitude is zero, cannot determine voice presence.";
    } else {
        // Improved voice detection logic
        double thresholdAmplitude = static_cast<double>(silenceFinder.avgAmplitude);
        if (!_inVoiceSegment) {
            voicePresent = thresholdAmplitude > (voiceStartThreshold * thresholdAmplitude);
        } else {
            voicePresent = thresholdAmplitude > (voiceEndThreshold * thresholdAmplitude);
        }
    }

    // Convert to uint8_t for accumulation
    std::vector<uint8_t> currentBuffer;
    currentBuffer.reserve(int16Buffer.size() * 2);
    for (const auto& sample : int16Buffer) {
        currentBuffer.push_back(sample & 0xFF);
        currentBuffer.push_back((sample >> 8) & 0xFF);
    }

    if (voicePresent) {
        if (!_inVoiceSegment) {
            _inVoiceSegment = true;
            _samplesSinceVoiceStart = 0;
        }
        _silentSamplesCount = 0;
        _accumulatedByteBuffer.insert(_accumulatedByteBuffer.end(), currentBuffer.begin(), currentBuffer.end());
        _samplesSinceVoiceStart += currentBuffer.size();
        
        // Check if we've reached 10 seconds while speaking
        if (_accumulatedByteBuffer.size() >= kTargetSamples) {
            RTC_LOG(LS_INFO) << "Pushing " << kTargetSamples/2 
                            << " samples to Whisper queue (continuous speech)";
            
            if (!_audioBuffer.write(_accumulatedByteBuffer.data(), kTargetSamples)) {
                RTC_LOG(LS_WARNING) << "Ring buffer overflow, data lost";
                handleOverflow();
            }
            
            // Keep the remainder
            if (_accumulatedByteBuffer.size() > kTargetSamples) {
                _accumulatedByteBuffer.erase(_accumulatedByteBuffer.begin(), 
                                           _accumulatedByteBuffer.begin() + kTargetSamples);
                _samplesSinceVoiceStart = _accumulatedByteBuffer.size();
            } else {
                _accumulatedByteBuffer.clear();
                _samplesSinceVoiceStart = 0;
            }
        }
    } else {
        _silentSamplesCount += currentBuffer.size();

        if (_inVoiceSegment && _silentSamplesCount >= kSilenceSamples) {
            _inVoiceSegment = false;

            // Send buffer if we have enough samples OR if we're in silence after voice
            if (_accumulatedByteBuffer.size() >= kTargetSamples || 
                (_accumulatedByteBuffer.size() >= kSampleRate * 2)) { // At least 1 second of speech
                
                size_t samplesTo = std::min(_accumulatedByteBuffer.size(), kTargetSamples);
                
                RTC_LOG(LS_INFO) << "Pushing " << samplesTo/2 
                                << " samples to Whisper queue (end of speech)";
                
                if (!_audioBuffer.write(_accumulatedByteBuffer.data(), samplesTo)) {
                    RTC_LOG(LS_WARNING) << "Ring buffer overflow, data lost";
                    handleOverflow();
                }
                
                if (_accumulatedByteBuffer.size() > samplesTo) {
                    _accumulatedByteBuffer.erase(_accumulatedByteBuffer.begin(), 
                                               _accumulatedByteBuffer.begin() + samplesTo);
                    _samplesSinceVoiceStart = _accumulatedByteBuffer.size();
                } else {
                    _accumulatedByteBuffer.clear();
                    _samplesSinceVoiceStart = 0;
                }
            }
            _silentSamplesCount = 0;
        }
    }
}

void WhisperTranscriber::handleOverflow() {
    _overflowCount++;
    if(_overflowCount > 10) {
        RTC_LOG(LS_INFO) << "Frequent buffer overflows, increasing buffer size";
        _audioBuffer.increaseWith(kRingBufferSizeIncrement);
        _overflowCount = 0;
    }
}
bool WhisperTranscriber::Start() {
    if (!_running) {
        _running = true;
        _processingThread = rtc::PlatformThread::SpawnJoinable(
            [this] {
              while (RunProcessingThread()) {
              }
            },
            "whisper_processing_thread",
            rtc::ThreadAttributes().SetPriority(rtc::ThreadPriority::kNormal));
    }

    return _running && !_processingThread.empty();
}

void WhisperTranscriber::Stop() {
    if (_running) {
        _running = false;
        
        _processingThread.Finalize();

        // Clear any remaining accumulated buffer
        _accumulatedByteBuffer.clear();
    }
}
