#pragma once

#include "icm42670_driver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace robobaton_demo::sensor_web {
// Preview-only anti-aliasing before telemetry decimation. Four unity-gain
// one-pole stages preserve DC/slow turns; they do not estimate attitude or bias.
class PreviewImuFilter {
 public:
  explicit PreviewImuFilter(uint32_t source_rate_hz = 800,
                            uint32_t telemetry_hz = 50) noexcept
      : pole_hz_(std::min({10.0, source_rate_hz * 0.2, telemetry_hz * 0.2})),
        angular_rate_(2.0 * 3.14159265358979323846 * pole_hz_) {}

  bool Observe(const icm42670_sample_t& sample) noexcept {
    const std::array<double, 6> input{sample.accel_mps2[0], sample.accel_mps2[1],
        sample.accel_mps2[2], sample.gyro_rps[0], sample.gyro_rps[1], sample.gyro_rps[2]};
    if (sample.struct_size != sizeof(sample) || sample.sample_timestamp_ns == 0 ||
        !std::all_of(input.begin(), input.end(), [](double v) { return std::isfinite(v); })) {
      timestamp_ = 0;
      stages_.back().fill(std::numeric_limits<double>::quiet_NaN());
      return false;
    }
    const uint64_t now = sample.sample_timestamp_ns;
    if (timestamp_ == 0 || sample.sample_sequence <= sequence_ || now <= timestamp_ ||
        now - timestamp_ > 100000000ULL) {
      for (auto& stage : stages_) stage = input;
    } else {
      const double step = angular_rate_ * (static_cast<double>(now - timestamp_) * 1e-9);
      const double alpha = step / (1.0 + step);
      const auto* previous = &input;
      for (auto& stage : stages_) {
        for (size_t axis = 0; axis < stage.size(); ++axis)
          stage[axis] += alpha * ((*previous)[axis] - stage[axis]);
        previous = &stage;
      }
    }
    timestamp_ = now;
    sequence_ = sample.sample_sequence;
    return true;
  }

  const std::array<double, 6>& Values() const noexcept { return stages_.back(); }
  double PoleHz() const noexcept { return pole_hz_; }

 private:
  double pole_hz_;
  double angular_rate_;
  uint64_t timestamp_ = 0;
  uint64_t sequence_ = 0;
  std::array<std::array<double, 6>, 4> stages_{};
};
}  // namespace robobaton_demo::sensor_web
