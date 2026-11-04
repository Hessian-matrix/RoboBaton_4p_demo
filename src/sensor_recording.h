#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "cam_demo_common.h"
#include "prrtsp_v2.h"
#include "imu_realtime.h"

namespace robobaton_demo {
class SensorBagRecorder;
class H264Mp4Recorder;

struct RecordingCommand {
  uint32_t action = 0;
  uint32_t format = 0;
  uint32_t storage = 0;
  uint32_t frame_skip = 0;
  uint32_t drop_min_interval = 10;
  uint64_t request_id = 0;
  uint64_t expected_session_id = 0;
};

// One lifecycle worker owns start/drain/publish. The admission gate protects
// borrowed callbacks only; disk I/O, joins and remux never hold that gate.
class SensorRecording final {
 public:
  explicit SensorRecording(const Options& options);
  ~SensorRecording();
  SensorRecording(const SensorRecording&) = delete;
  SensorRecording& operator=(const SensorRecording&) = delete;
  void AdoptStartup(SensorBagRecorder* bag, H264Mp4Recorder* mp4,
                    const std::string& path);
  void SetSourceReady() noexcept;
  void Submit(const RecordingCommand& command);
  std::string StatusJson() const;
  bool Saving() const noexcept;
  void ObserveFrameSet(const sc132_frame_set_t& set) noexcept;
  void ObserveImu(const OutputImuSample& sample) noexcept;
  void ObserveEncoded(int id, const prrtsp_encoded_frame_v2& frame) noexcept;
  // Called after camera consumers and IMU are quiescent, before SC blocking stop.
  void Shutdown(bool source_success, const icm42670_runtime_health_t* health = nullptr) noexcept;
  static void EncodedObserver(int id, const prrtsp_encoded_frame_v2& frame, void* user) noexcept;
 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace robobaton_demo
