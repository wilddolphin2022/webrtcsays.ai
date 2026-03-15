#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "src/modules/third_party/whillats/src/talking_face.h"
#include "src/modules/third_party/whillats/src/whillats.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: debug_talking_face <input-image> <output-bmp>\n";
    return 1;
  }

  TalkingFace face;
  if (!face.loadImage(argv[1])) {
    std::cerr << "failed to load image: " << argv[1] << "\n";
    return 2;
  }

  face.setOutputSize(640, 480);

  std::vector<int16_t> samples(16000 / 10);
  for (size_t i = 0; i < samples.size(); ++i) {
    double t = static_cast<double>(i) / 16000.0;
    samples[i] = static_cast<int16_t>(std::sin(2.0 * M_PI * 440.0 * t) * 12000.0);
  }
  face.feedAudio(samples.data(), samples.size());

  YUVData yuv;
  if (!face.renderFrame(yuv)) {
    std::cerr << "renderFrame returned false\n";
    return 3;
  }

  if (!save_yuv_as_bmp(yuv, argv[2])) {
    std::cerr << "failed to save bmp: " << argv[2] << "\n";
    return 4;
  }

  std::cout << "wrote " << argv[2] << "\n";
  return 0;
}
