#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "cam_demo_common.h"
#include "prrtsp_v2.h"

namespace robobaton_demo {

struct H264Mp4RecorderStats {
  uint64_t encoded_frames_selected = 0U;
  uint64_t encoded_frames_admitted = 0U;
  uint64_t encoded_frames_written = 0U;
  uint64_t encoded_frames_dropped = 0U;
  std::array<uint64_t, kMaxChannels> encoded_frames_selected_by_camera{};
  std::array<uint64_t, kMaxChannels> encoded_frames_admitted_by_camera{};
  std::array<uint64_t, kMaxChannels> encoded_frames_written_by_camera{};
  std::array<uint64_t, kMaxChannels> encoded_frames_dropped_by_camera{};
  uint64_t encoded_queue_capacity = 0U;
  uint64_t encoded_queue_current_depth = 0U;
  uint64_t encoded_queue_peak_depth = 0U;
  uint64_t encoded_queue_full_drops = 0U;
  uint64_t encoded_queue_byte_capacity = 0U;
  uint64_t encoded_queue_current_bytes = 0U;
  uint64_t encoded_queue_byte_high_watermark = 0U;
  uint64_t encoded_queue_byte_full_drops = 0U;
  uint64_t encoded_bytes_written = 0U;
  uint64_t imu_samples_admitted = 0U;
  uint64_t imu_samples_written = 0U;
  uint64_t imu_samples_dropped = 0U;
  uint64_t imu_queue_peak_depth = 0U;
};

struct H264Mp4SourceHealth {
  uint64_t samples = 0U;
  uint64_t invalid_samples = 0U;
  uint64_t timestamp_duplicates = 0U;
  uint64_t timestamp_regressions = 0U;
  uint64_t sequence_gaps = 0U;
  uint64_t sequence_duplicates = 0U;
  uint64_t sequence_regressions = 0U;
  uint32_t gpio_event_gap_count = 0U;
  uint32_t fifo_overflow_count = 0U;
  uint64_t timing_sample_drops = 0U;
  uint32_t max_timestamp_uncertainty_us = 0U;
  uint32_t max_consecutive_drops = 0U;
  uint32_t uncertainty_over_200_drops = 0U;
  uint32_t producer_session_generation = 0U;
  uint64_t producer_published_samples = 0U;
  bool producer_final_health_valid = false;
  bool mapper_counter_valid = false;
};

enum class H264Mp4FinishOutcome {
  kPublishedComplete,
  kPublishedPartial,
  kAborted,
  kCleanupIncomplete,
};

const char* H264Mp4FinishOutcomeName(H264Mp4FinishOutcome outcome) noexcept;

struct H264Mp4FinishResult {
  H264Mp4FinishOutcome outcome = H264Mp4FinishOutcome::kAborted;
  bool data_complete = false;
  bool cleanup_complete = true;
  std::string published_path;
  std::string error;

  operator bool() const noexcept {
    return cleanup_complete &&
           (outcome == H264Mp4FinishOutcome::kPublishedComplete ||
            outcome == H264Mp4FinishOutcome::kPublishedPartial);
  }
};

// Optional sidecar that stores the already encoded RTSP H.264 stream without re-encoding.
class H264Mp4Recorder final {
 public:
  H264Mp4Recorder();
  ~H264Mp4Recorder();

  H264Mp4Recorder(const H264Mp4Recorder&) = delete;
  H264Mp4Recorder& operator=(const H264Mp4Recorder&) = delete;

  void Start(const Options& options, const std::string& output_directory);
  void ObserveEncodedFrame(int camera_id,
                           const prrtsp_encoded_frame_v2& frame) noexcept;
  void ObserveImu(const icm42688_sample_t& sample) noexcept;
  void SetSourceHealth(const H264Mp4SourceHealth& health) noexcept;
  H264Mp4FinishResult Finish(bool session_success) noexcept;
  void Abort() noexcept;

  bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
  bool HasFatalError() const noexcept;
  std::string ErrorMessage() const;
  H264Mp4RecorderStats SnapshotStats() const;

#ifdef RELEASE008_TESTING
  void PauseWriterForTest() noexcept;
  void ResumeWriterForTest() noexcept;
  void SetEncodedQueueCapacityForTest(std::size_t capacity) noexcept;
  void SetEncodedQueueByteCapacityForTest(uint64_t byte_capacity) noexcept;
  void SetRemuxTimeoutForTest(uint32_t timeout_ms,
                              uint32_t terminate_grace_ms) noexcept;
  uint32_t RemuxTimeoutForBytesForTest(uint64_t raw_bytes) const noexcept;
  void FailPublicationReceiptAfterRenameForTest() noexcept;
  void FailFinalDirectoryQuarantineForTest() noexcept;
  void FailQuarantinedSessionParentSyncForTest() noexcept;
  void FailParentDirectorySyncAfterRenameForTest() noexcept;
  void FailPublicationMarkerRemovalForTest() noexcept;
  void FailSessionStatusRewriteWriteForTest() noexcept;
  void FailSessionStatusRewriteFsyncForTest() noexcept;
  void FailSessionStatusRewriteRenameForTest() noexcept;
  void FailSessionStatusRewriteDirectorySyncForTest() noexcept;
  void FailSessionStatusCloseForTest() noexcept;
  void FailPublicationReceiptCloseForTest() noexcept;
  void FailOutputCloseForTest() noexcept;
#endif

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> enabled_{false};
};

}  // namespace robobaton_demo
