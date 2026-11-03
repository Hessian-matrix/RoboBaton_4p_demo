#pragma once

#include <memory>
#include "cam_demo_common.h"
#include "prrtsp_v2.h"

namespace robobaton_demo {
class MediaChannels;
class SensorRecording;

// Owns only this application's optional preview process and bounded IPC buffers.
class SensorWebRuntime final {
 public:
  SensorWebRuntime();
  ~SensorWebRuntime();
  SensorWebRuntime(const SensorWebRuntime&) = delete;
  SensorWebRuntime& operator=(const SensorWebRuntime&) = delete;
  bool Start(const Options& options, const MediaChannels* media) noexcept;
  void SetRecording(SensorRecording* recording) noexcept;
  void BeginShutdown() noexcept;
  void Stop() noexcept;
  void ObserveEncoded(int camera_id, const prrtsp_encoded_frame_v2& frame) noexcept;
  void ObserveImu(const icm42670_sample_t& sample, uint64_t arrival_ns) noexcept;
  void PublishStatus(bool capture_running, bool encoding_running, bool saving,
                     bool source_failed, bool rtsp_degraded) noexcept;
  static void EncodedObserver(int camera_id, const prrtsp_encoded_frame_v2& frame,
                              void* user) noexcept;
 private:
  class Impl;
  SensorRecording* recording_ = nullptr;
  std::unique_ptr<Impl> impl_;
};

}  // namespace robobaton_demo
