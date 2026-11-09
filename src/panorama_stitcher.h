#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace robobaton_demo {

struct PanoramaCamera {
  uint32_t width = 0, height = 0;
  double fx = 0, fy = 0, cx = 0, cy = 0;
  std::array<double, 4> distortion{};
  // Column-vector convention: p_cam = rotation * p_imu + translation_m.
  std::array<double, 9> rotation{};
  std::array<double, 3> translation_m{};
};
struct PanoramaCalibration {
  std::array<PanoramaCamera, 4> cameras;
  std::string acceptance;
};
PanoramaCalibration LoadPanoramaCalibration(const std::string& path);

struct PanoramaProjection {
  uint32_t width = 1600, height = 600;
  double elevation_min_deg = -60, elevation_max_deg = 60;
  double yaw_deg = 0;
  // Zero is infinity (rotation only); positive values are IMU-centred sphere radii.
  double distance_m = 0;
  // Camera A diagonal FOV is 193.8 degrees. Pixel bounds are checked independently.
  double max_theta_deg = 96.9;
};
struct PanoramaImage {
  uint32_t width = 0, height = 0, stride = 0;
  const uint8_t* rgb = nullptr;
  const uint8_t* y = nullptr;
  const uint8_t* uv = nullptr;
  size_t y_size = 0, uv_size = 0;
};

class PanoramaStitcher final {
 public:
  PanoramaStitcher(const PanoramaCalibration&, const PanoramaProjection&);
  ~PanoramaStitcher();
  // Inputs must share one captured frame-set. RGB and NV12 use the same geometry.
  // Workspace/output are reused; returned storage remains owned by this instance.
  // Render is single-caller; its persistent workers finish before inputs return.
  const std::vector<uint8_t>& Render(const std::array<PanoramaImage, 4>&);
  double coverage_percent() const noexcept { return coverage_percent_; }
  double horizon_coverage_percent() const noexcept { return horizon_coverage_percent_; }

 private:
  struct Sample {
    uint32_t output;
    uint32_t x_q8, y_q8;
    float weight;
  };
  struct Seam {
    uint8_t left = 0, right = 0;
    uint32_t start = 0, width = 0;
    std::array<std::vector<Sample>, 2> maps;
    std::array<std::vector<std::array<uint8_t, 3>>, 2> colors;
    std::vector<uint8_t> writable;
    std::vector<float> costs, filtered, quality_loss, centre_costs, column_sums;
    std::vector<double> accumulated, next;
    std::vector<float> illumination, correction_weight;
    uint32_t illumination_width = 0, illumination_height = 0;
    std::vector<int8_t> parents;
    std::vector<int> path;
    bool initialized = false;
  };
  void RenderSeam(Seam&, const std::array<PanoramaImage, 4>&);
  void BalanceExposure(const std::array<PanoramaImage, 4>&);
  void RenderJob(size_t, const std::array<PanoramaImage, 4>&);
  PanoramaCalibration calibration_;
  PanoramaProjection projection_;
  std::array<std::vector<Sample>, 4> maps_;
  std::vector<Seam> seams_;
  std::vector<uint8_t> rgb_;
  std::array<float, 4> gains_{1, 1, 1, 1};
  double coverage_percent_ = 0, horizon_coverage_percent_ = 0;
  struct Workers;
  std::unique_ptr<Workers> workers_;
};

}  // namespace robobaton_demo
