#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#if defined(_WIN32)
#if defined(ICM42688_X5_BUILDING_LIBRARY)
#define ICM42688_X5_API __declspec(dllexport)
#else
#define ICM42688_X5_API __declspec(dllimport)
#endif
#else
#define ICM42688_X5_API __attribute__((visibility("default")))
#endif

namespace icm42688_x5 {

struct RawImuSample {
  int16_t temperature_raw = 0;
  int16_t accel_x_raw = 0;
  int16_t accel_y_raw = 0;
  int16_t accel_z_raw = 0;
  int16_t gyro_x_raw = 0;
  int16_t gyro_y_raw = 0;
  int16_t gyro_z_raw = 0;
};

struct ImuSample {
  uint64_t host_timestamp_ns = 0;
  double temperature_c = 0.0;
  double accel_x_mps2 = 0.0;
  double accel_y_mps2 = 0.0;
  double accel_z_mps2 = 0.0;
  double gyro_x_rps = 0.0;
  double gyro_y_rps = 0.0;
  double gyro_z_rps = 0.0;
  RawImuSample raw;
};

enum class ReadMode {
  Fifo,
  Direct,
};

struct DriverConfig {
  uint32_t sample_rate_hz = 1000;
  ReadMode read_mode = ReadMode::Fifo;
  uint32_t fifo_watermark_samples = 8;
};

using SampleCallback = std::function<void(const ImuSample&)>;

// 2026-05-20 修改原因：公开 demo 只暴露稳定的用户态 API，底层 SPI/FIFO 细节由二进制库封装。
class ICM42688_X5_API Driver {
 public:
  explicit Driver(DriverConfig config);
  ~Driver();

  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;

  void RegisterCallback(SampleCallback callback);
  void Start();
  void Stop();
  bool IsRunning() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace icm42688_x5
