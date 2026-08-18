#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cam_demo_common.h"
#include "rosbag_v2_writer.h"
#include "x5_jpeg_encoder.h"

namespace robobaton_demo {

constexpr size_t kSensorBagLatencyBucketCount = 32U;

struct SensorBagLatencyStats {
  uint64_t count = 0U;
  uint64_t total_ns = 0U;
  uint64_t max_ns = 0U;
  std::array<uint64_t, kSensorBagLatencyBucketCount> buckets{};
};

uint64_t SensorBagLatencyAverageNs(const SensorBagLatencyStats& stats) noexcept;
uint64_t SensorBagLatencyPercentileUpperNs(const SensorBagLatencyStats& stats,
                                           uint32_t percentile) noexcept;

struct SensorBagRecorderStats {
  uint64_t image_frames = 0U;
  uint64_t imu_samples = 0U;
  uint64_t source_frame_sets_seen = 0U;
  uint64_t recorder_selected_groups = 0U;
  uint64_t recorder_admitted_groups = 0U;
  uint64_t recorder_dropped_groups = 0U;
  uint64_t recorder_written_groups = 0U;
  uint64_t recorder_imu_admitted = 0U;
  uint64_t recorder_imu_written = 0U;
  uint64_t imu_queue_peak_depth = 0U;
  uint64_t imu_queue_full_rejects = 0U;
  uint64_t frame_queue_peak_depth = 0U;
  uint64_t frame_queue_full_rejects = 0U;
  uint64_t encoded_queue_peak_depth = 0U;
  uint64_t encoded_queue_full_rejects = 0U;
  uint64_t encoded_queue_oldest_evicted_groups = 0U;
  uint64_t encoded_queue_oldest_evicted_frames = 0U;
  uint64_t encoded_queue_oldest_evicted_bytes = 0U;
  uint64_t writer_image_backlog_peak_depth = 0U;
  uint64_t frame_queue_capacity = 0U;
  uint64_t frame_queue_high_watermark = 0U;
  uint64_t frame_queue_at_capacity_dwell_ns = 0U;
  uint64_t frame_queue_at_capacity_events = 0U;
  uint64_t encoded_queue_capacity = 0U;
  uint64_t encoded_queue_high_watermark = 0U;
  uint64_t encoded_queue_at_capacity_dwell_ns = 0U;
  uint64_t encoded_queue_at_capacity_events = 0U;
  uint64_t imu_queue_capacity = 0U;
  uint64_t imu_queue_high_watermark = 0U;
  uint64_t imu_queue_at_capacity_dwell_ns = 0U;
  uint64_t imu_queue_at_capacity_events = 0U;
  uint64_t frame_metadata_messages = 0U;
  uint64_t jpeg_bytes = 0U;
  uint64_t nv12_copy_bytes = 0U;
  uint64_t nv12_copy_bulk_plane_count = 0U;
  uint64_t nv12_copy_bulk_bytes = 0U;
  uint64_t nv12_copy_row_count = 0U;
  uint64_t nv12_copy_row_bytes = 0U;
  uint64_t write_order_wait_max_ns = 0U;
  uint64_t writer_mutex_wait_max_ns = 0U;
  uint64_t writer_mutex_hold_max_ns = 0U;
  SensorBagLatencyStats nv12_copy_latency;
  SensorBagLatencyStats jpeg_encode_latency;
  SensorBagLatencyStats write_order_wait_latency;
  SensorBagLatencyStats image_writer_wait_latency;
  SensorBagLatencyStats image_writer_hold_latency;
  SensorBagLatencyStats imu_writer_wait_latency;
  SensorBagLatencyStats imu_writer_hold_latency;
  std::array<uint64_t, kMaxChannels> image_frames_by_camera{};
  uint64_t first_image_timestamp_ns = 0U;
  uint64_t last_image_timestamp_ns = 0U;
  bool has_image_timestamp = false;
  RosbagV2WriterStats writer_stats;
};

enum class SensorBagFinishOutcome {
  kPublishedComplete,
  kPublishedPartial,
  kAborted,
  kCleanupIncomplete,
  kPublishedDurabilityUnproven,
};

const char* SensorBagFinishOutcomeName(SensorBagFinishOutcome outcome) noexcept;

struct SensorBagFinishResult {
  SensorBagFinishOutcome outcome = SensorBagFinishOutcome::kAborted;
  bool data_complete = false;
  bool cleanup_complete = true;
  std::string session_uuid;
  std::string published_path;
  std::string quarantine_path;
  std::string error;

  operator bool() const noexcept {
    return cleanup_complete && (outcome == SensorBagFinishOutcome::kPublishedComplete ||
                                outcome == SensorBagFinishOutcome::kPublishedPartial);
  }
};

struct SensorBagFrameJob {
  int camera_id = 0;
  uint64_t sequence = 0U;
  uint64_t frame_id = 0U;
  uint64_t group_id = 0U;
  uint64_t group_timestamp_ns = 0U;
  uint64_t record_order = 0U;
  QueuedFrame raw_frame;
  uint64_t camera_timestamp_ns = 0U;
  X5JpegInputSlot* jpeg_slot = nullptr;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
};


class SensorBagRecorder final {
 public:
  SensorBagRecorder() = default;
  ~SensorBagRecorder();

  SensorBagRecorder(const SensorBagRecorder&) = delete;
  SensorBagRecorder& operator=(const SensorBagRecorder&) = delete;

  void Start(const Options& options, const std::string& bag_path);
  enum class FrameSetAdmissionStatus {
    kAccepted,
    kSkipped,
    kStopped,
    kQueueFull,
    kFatal,
  };
  FrameSetAdmissionStatus TryAcceptFrameSet(const sc132_frame_set_t& frame_set) noexcept;
  void ObserveFrame(const QueuedFrame& frame);
  void ObserveFrameSet(const sc132_frame_set_t& frame_set);
  void ObserveImu(const icm42688_sample_t& sample);
  SensorBagFinishResult Finish(bool session_success) noexcept;
  void Abort() noexcept;

  bool enabled() const noexcept { return enabled_; }
  bool HasFatalError() const noexcept;
  std::string ErrorMessage() const;
  SensorBagRecorderStats SnapshotStats() const;
#ifdef RELEASE008_TESTING
  void PauseWriterForTest() noexcept;
  void ResumeWriterForTest() noexcept;
  void SetEncodedAdmissionHighWatermarkForTest(size_t high_watermark) noexcept;
  void FailNextPayloadPoolRecycleForTest() noexcept;
  uint64_t WriterWakeupGenerationForTest() noexcept;
#endif
 private:
  static constexpr size_t kQueueCapacity = 64U;
  static constexpr size_t kFrameWorkerCount = kMaxChannels;
  static constexpr size_t kReusableJobCapacity = kQueueCapacity + kFrameWorkerCount;
  static constexpr size_t kEncodedQueueCapacity = 4096U;
  static constexpr size_t kMaxOldestFrameSetsEvictedPerAdmission = 2U;
  // 保留更大的重排窗口，吸收 writer 的秒级抖动，避免可恢复 backlog 过早转成整组丢帧。
  static constexpr size_t kEncodedAdmissionHighWatermark =
      kEncodedQueueCapacity * 15U / 16U;
  static constexpr size_t kWriterImagePrefetchLimit = 8U;
  static constexpr size_t kReusablePayloadCapacity =
      kReusableJobCapacity + kWriterImagePrefetchLimit;
  static constexpr size_t kWriterImageBurstBeforeImu = 8U;
  static constexpr size_t kWriterImuDrainBatch = 64U;
  static constexpr size_t kImuQueueCapacity = 8192U;
  static constexpr size_t kWriterImuHighWatermark = kImuQueueCapacity / 2U;
  static constexpr size_t kWriterImuHighWatermarkDrainBatch = 256U;
  static constexpr uint32_t kJpegQuality = 80U;
  struct QueueAtCapacityTracker {
    bool active = false;
    uint64_t start_ns = 0U;
  };

  struct EncodedFrameJob {
    SensorBagFrameJob job;
    std::vector<uint8_t> image_payload;
    std::vector<uint8_t> metadata_payload;
    size_t jpeg_size = 0U;
    uint64_t queued_timestamp_ns = 0U;
  };

  struct EncodedFrameSlot {
    uint64_t record_order = 0U;
    bool occupied = false;
    bool evicted = false;
    EncodedFrameJob job;
  };
  struct PendingEncodedWrite {
    EncodedFrameJob encoded;
    uint64_t order_wait_ns = 0U;
  };



  void WorkerEntry(int camera_id) noexcept;
  void WriterEntry() noexcept;
  void RequestStop() noexcept;
  void StopWorkers() noexcept;
  bool PopFrame(int camera_id, SensorBagFrameJob* job) noexcept;
  bool PopNextEncodedFrame(EncodedFrameJob* encoded, uint64_t* order_wait_ns) noexcept;
  size_t PopImuBatch(icm42688_sample_t* samples, size_t max_samples) noexcept;
  bool WriterInputReady() noexcept;
  bool WriterInputsClosed() noexcept;
  bool HasClosedEncodedOrderGap() noexcept;
  void ReleaseFrameJob(SensorBagFrameJob* job) noexcept;
  EncodedFrameJob EncodeFrame(SensorBagFrameJob* job);
  EncodedFrameJob AcquireEncodedFrameJob();
  void ReleaseEncodedFrameJob(EncodedFrameJob* encoded) noexcept;
  void PublishEncodedFrame(EncodedFrameJob encoded);
  void WriteEncodedFrameToBag(const EncodedFrameJob& encoded, uint64_t order_wait_ns);
  void WriteImuSample(const icm42688_sample_t& sample, std::vector<uint8_t>* payload);
  void ObserveCopyTiming(uint64_t copy_ns,
                         const X5JpegNv12CopyResult& copy_result) noexcept;
  void ObserveJpegTiming(uint64_t encode_ns) noexcept;
  void ObserveWriterTiming(uint64_t order_wait_ns, uint64_t writer_wait_ns,
                           uint64_t writer_hold_ns, bool image_write) noexcept;
  bool EncodedBackpressureActive() noexcept;
  bool MakeEncodedAdmissionRoomByEvictingOldestReadyFrameSets() noexcept;
  void RecordEncodedBackpressureReject() noexcept;
  bool HasRecordedDrops() const noexcept;
  void RecordSourceFrameSetSeen() noexcept;
  void RecordSelectedGroup() noexcept;
  void RecordAdmittedGroup() noexcept;
  void UpdateWrittenGroupCountLocked() noexcept;
  void UpdateFrameQueueAccountingLocked(size_t depth) noexcept;
  void UpdateEncodedQueueAccountingLocked(size_t depth) noexcept;
  void UpdateImuQueueAccountingLocked(size_t depth) noexcept;
  static void UpdateQueueAtCapacity(bool at_capacity, QueueAtCapacityTracker* tracker,
                                    uint64_t* dwell_ns, uint64_t* events) noexcept;
  RosbagTime RecordDataStamp(uint64_t preferred_time_ns) noexcept;
  void WriteCameraInfoMessage(int camera_id, RosbagTime stamp);
  void EnsureSessionConfigWritten(RosbagTime stamp);
  void EnsureCameraInfoWritten(int camera_id, RosbagTime stamp);
  void WriteSessionStatus(SensorBagFinishOutcome outcome, bool data_complete,
                          const std::string& published_path,
                          const std::string& session_uuid);
  void SetFatalError(const std::string& message) noexcept;
  uint32_t ImageConnection(int camera_id) const;
  uint32_t CameraInfoConnection(int camera_id) const;
  uint32_t FrameMetadataConnection(int camera_id) const;
  bool ShouldRecordGroup(uint64_t group_id, uint64_t group_timestamp_ns);
  struct RecordSelection {
    uint64_t group_id = 0U;
    bool selected = false;
  };

  RosbagV2Writer writer_;
  X5JpegEncoder jpeg_encoder_;
  mutable std::mutex stats_mutex_;
  SensorBagRecorderStats stats_;

  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::deque<SensorBagFrameJob> queue_;
  std::vector<SensorBagFrameJob> reusable_jobs_;
  std::mutex payload_pool_mutex_;
  std::vector<EncodedFrameJob> reusable_encoded_payloads_;
  std::vector<std::thread> workers_;
  std::mutex imu_queue_mutex_;
  std::condition_variable imu_queue_condition_;
  std::vector<icm42688_sample_t> imu_queue_;
  std::mutex encoded_mutex_;
  std::condition_variable encoded_condition_;
  std::vector<EncodedFrameSlot> encoded_slots_;
  size_t encoded_queue_count_ = 0U;
  size_t encoded_admission_high_watermark_ = kEncodedAdmissionHighWatermark;
  std::thread writer_worker_;
  std::mutex writer_wakeup_mutex_;
  std::condition_variable writer_condition_;
  uint64_t writer_wakeup_generation_ = 0U;
#ifdef RELEASE008_TESTING
  std::atomic<bool> writer_paused_for_test_{false};
  std::atomic<bool> fail_next_payload_pool_recycle_for_test_{false};
#endif
  uint64_t next_frame_order_ = 0U;
  uint64_t next_write_order_ = 0U;
  bool encoded_input_closed_ = false;
  size_t imu_queue_head_ = 0U;
  size_t imu_queue_count_ = 0U;
  std::atomic<bool> stopping_{false};
  bool enabled_ = false;
  bool finished_ = false;
  uint32_t camera_mask_ = 0U;
  uint32_t record_target_fps_ = 0U;
  Options options_;
  std::vector<uint32_t> image_connections_;
  const FrozenSystemClock* system_clock_ = nullptr;
  std::vector<uint32_t> camera_info_connections_;
  std::vector<uint32_t> frame_metadata_connections_;
  std::vector<bool> camera_info_written_;
  uint32_t imu_connection_ = 0U;
  uint32_t session_config_connection_ = 0U;
  uint32_t session_status_connection_ = 0U;
  bool session_config_written_ = false;
  bool has_last_data_record_time_ = false;
  uint64_t last_data_record_time_ns_ = 0U;
  std::mutex record_selection_mutex_;
  std::deque<RecordSelection> record_selections_;
  bool next_group_should_record_ = true;
  SensorBagFinishResult finish_result_;
  QueueAtCapacityTracker frame_queue_at_capacity_;
  QueueAtCapacityTracker encoded_queue_at_capacity_;
  QueueAtCapacityTracker imu_queue_at_capacity_;

  std::atomic<bool> fatal_error_{false};
  mutable std::mutex error_mutex_;
  std::string error_message_;
};

}  // namespace robobaton_demo
