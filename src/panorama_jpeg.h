#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace robobaton_demo {
struct PanoramaRgbImage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> pixels;
};
PanoramaRgbImage ReadPanoramaJpeg(const std::string& path);
std::shared_ptr<const std::vector<uint8_t>> EncodePanoramaJpeg(
    const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height);
}  // namespace robobaton_demo
