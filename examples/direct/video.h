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

#ifndef DIRECT_STATIC_H_
#define DIRECT_STATIC_H_

#include <memory>
#include <cstring>

#include "api/video/video_source_interface.h"
#include "media/base/fake_frame_source.h"
#include "media/base/video_broadcaster.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/task_queue_for_test.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "rtc_base/time_utils.h"
#include "pc/video_track_source.h"

#include "api/video/i420_buffer.h"

#if defined(WEBRTC_SPEECH_DEVICES) 
#include "modules/third_party/whillats/src/whillats.h"
#include "modules/audio_device/speech/speech_audio_device_factory.h"
#endif

namespace webrtc {
// StaticPeriodic video source no longer used; class definitions removed to reduce footprint.

class EchoVideoTrackSource : public webrtc::VideoTrackSource,
                            public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  EchoVideoTrackSource()
      : webrtc::VideoTrackSource(/*remote=*/false) {
     SetState(webrtc::MediaSourceInterface::kLive);
   }

  // rtc::VideoSinkInterface implementation – called with frames from the
  // *remote* video track we attach to.
  void OnFrame(const webrtc::VideoFrame& frame) override {
    broadcaster_.OnFrame(frame);
  }
  void OnDiscardedFrame() override {}

  // Expose wants aggregation from broadcaster
  rtc::VideoSinkWants wants() const { return broadcaster_.wants(); }

 protected:
  // webrtc::VideoTrackSource implementation – this is what the WebRTC encoder
  // pulls frames from.
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return &broadcaster_;
  }

 private:
  rtc::VideoBroadcaster broadcaster_;
};

#if defined(WEBRTC_SPEECH_DEVICES) 
// Simple video sink that logs frame information to the console
class LlamaVideoRenderer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
  // Function to check if a WebRTC VideoFrame (I420 format) is black
  bool isVideoFrameBlack(const webrtc::VideoFrame& frame, int threshold = 16) {
      // Get the I420 buffer from the VideoFrame
      rtc::scoped_refptr<webrtc::I420BufferInterface> buffer = frame.video_frame_buffer()->ToI420();
      if (!buffer) {
          return false; // Invalid buffer
      }

      // Get Y plane data, width, height, and stride
      const uint8_t* yPlane = buffer->DataY();
      int width = buffer->width();
      int height = buffer->height();
      int stride = buffer->StrideY();

      // Iterate through the Y plane
      for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
              // Access Y value at position (x, y)
              uint8_t yValue = yPlane[y * stride + x];
              // If any Y value is above the threshold, the frame is not black
              if (yValue > threshold) {
                  return false;
              }
          }
      }

      // All Y values are below or equal to the threshold
      return true;
  }

 public:
  void set_is_llama(bool is_llama) { is_llama_ = is_llama; }
  bool is_llama() { return is_llama_; }
  
  void OnFrame(const webrtc::VideoFrame& frame) {

    if (!isVideoFrameBlack(frame)) {
      rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer(
          frame.video_frame_buffer());
      RTC_LOG(LS_VERBOSE) << "Received video frame (" << buffer->type() << ") "
                        << frame.width() << "x" << frame.height()
                        << " timestamp=" << frame.timestamp_us();

      // Convert the frame to I420 format and populate YUVData
      rtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer = buffer->ToI420();
      if (i420_buffer) {
        size_t y_size = i420_buffer->StrideY() * i420_buffer->height();
        size_t uv_size = i420_buffer->StrideU() * i420_buffer->ChromaHeight();

        YUVData yuv_data;
        yuv_data.width = i420_buffer->width();
        yuv_data.height = i420_buffer->height();
        yuv_data.y_size = y_size;
        yuv_data.uv_size = uv_size;
        yuv_data.y = std::make_unique<uint8_t[]>(y_size);
        std::memcpy(yuv_data.y.get(), i420_buffer->DataY(), y_size);
        yuv_data.u = std::make_unique<uint8_t[]>(uv_size);
        std::memcpy(yuv_data.u.get(), i420_buffer->DataU(), uv_size);
        yuv_data.v = std::make_unique<uint8_t[]>(uv_size);
        std::memcpy(yuv_data.v.get(), i420_buffer->DataV(), uv_size);

        if (is_llama_ && SpeechAudioDeviceFactory::llama()) {
          // Send video frame to be queued for later use with prompts
          SpeechAudioDeviceFactory::llama()->receiveVideoFrame(yuv_data);
          }
      }

      received_frame_ = true;
    } else {
      RTC_LOG(LS_INFO) << "Received black frame!";
    }
  }
 private:
  bool is_llama_ = false;
  bool received_frame_ = false;
};
#endif //WEBRTC_SPEECH_DEVICES

}  // namespace webrtc

#endif  // DIRECT_STATIC_H_