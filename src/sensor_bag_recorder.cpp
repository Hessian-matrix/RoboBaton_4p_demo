#include "sensor_bag_recorder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace robobaton_demo {
namespace {

constexpr uint32_t kMaxRosSeq = std::numeric_limits<uint32_t>::max();
constexpr size_t kRecordSelectionCacheCapacity = 256U;
constexpr int kRecorderEncodeWorkerNice = 10;
constexpr int kRecorderWriterNice = 0;

void ApplyRecorderWorkerPriority(int nice_value) noexcept {
#if defined(__linux__)
  const pid_t thread_id = static_cast<pid_t>(::syscall(SYS_gettid));
  if (thread_id > 0) {
    // record-bag 编码线程让出 CPU 给采集链路，顺序写盘线程保持普通优先级以避免 backlog 长时间顶满。
    static_cast<void>(::setpriority(PRIO_PROCESS, thread_id, nice_value));
  }
#endif
}

void NotifyWriter(std::mutex* mutex, std::condition_variable* condition,
                  uint64_t* generation) noexcept {
  if (mutex == nullptr || condition == nullptr || generation == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(*mutex);
    ++(*generation);
  }
  condition->notify_all();
}

void NotifyWriterOne(std::mutex* mutex, std::condition_variable* condition,
                     uint64_t* generation) noexcept {
  if (mutex == nullptr || condition == nullptr || generation == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(*mutex);
    ++(*generation);
  }
  condition->notify_one();
}




const char kCompressedImageDefinition[] =
    "std_msgs/Header header\n"
    "string format\n"
    "uint8[] data\n"
    "================================================================================\n"
    "MSG: std_msgs/Header\n"
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n";

const char kImuDefinition[] =
    "std_msgs/Header header\n"
    "geometry_msgs/Quaternion orientation\n"
    "float64[9] orientation_covariance\n"
    "geometry_msgs/Vector3 angular_velocity\n"
    "float64[9] angular_velocity_covariance\n"
    "geometry_msgs/Vector3 linear_acceleration\n"
    "float64[9] linear_acceleration_covariance\n"
    "================================================================================\n"
    "MSG: std_msgs/Header\n"
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n"
    "float64 w\n"
    "================================================================================\n"
    "MSG: geometry_msgs/Vector3\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n";

const char kCameraInfoDefinition[] =
    "std_msgs/Header header\n"
    "uint32 height\n"
    "uint32 width\n"
    "string distortion_model\n"
    "float64[] D\n"
    "float64[9] K\n"
    "float64[9] R\n"
    "float64[12] P\n"
    "uint32 binning_x\n"
    "uint32 binning_y\n"
    "sensor_msgs/RegionOfInterest roi\n"
    "================================================================================\n"
    "MSG: std_msgs/Header\n"
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: sensor_msgs/RegionOfInterest\n"
    "uint32 x_offset\n"
    "uint32 y_offset\n"
    "uint32 height\n"
    "uint32 width\n"
    "bool do_rectify\n";

const char kStringDefinition[] = "string data\n";

constexpr size_t kInitialJpegOutputBytes = 256U * 1024U;

std::string CameraFrameId(int camera_id) {
  return "robobaton_camera" + std::to_string(camera_id) + "_optical";
}

uint32_t SequenceToRosSeq(uint64_t sequence) noexcept {
  return sequence > kMaxRosSeq ? kMaxRosSeq : static_cast<uint32_t>(sequence);
}

uint32_t ImagePersistenceFps(int fps, uint32_t record_frame_skip) noexcept {
  if (fps <= 0) {
    return 0U;
  }
  return static_cast<uint32_t>(fps) / (record_frame_skip + 1U);
}

constexpr uint64_t kLatencyFirstBucketNs = 1000U;

uint64_t SaturatingAdd(uint64_t lhs, uint64_t rhs) noexcept {
  return rhs > std::numeric_limits<uint64_t>::max() - lhs
             ? std::numeric_limits<uint64_t>::max()
             : lhs + rhs;
}

size_t LatencyBucketIndex(uint64_t latency_ns) noexcept {
  uint64_t upper_bound_ns = kLatencyFirstBucketNs;
  size_t index = 0U;
  while (latency_ns > upper_bound_ns && index + 1U < kSensorBagLatencyBucketCount) {
    upper_bound_ns = upper_bound_ns > std::numeric_limits<uint64_t>::max() / 2U
                         ? std::numeric_limits<uint64_t>::max()
                         : upper_bound_ns * 2U;
    ++index;
  }
  return index;
}

uint64_t LatencyBucketUpperBoundNs(size_t index) noexcept {
  uint64_t upper_bound_ns = kLatencyFirstBucketNs;
  for (size_t i = 0U; i < index; ++i) {
    upper_bound_ns = upper_bound_ns > std::numeric_limits<uint64_t>::max() / 2U
                         ? std::numeric_limits<uint64_t>::max()
                         : upper_bound_ns * 2U;
  }
  return upper_bound_ns;
}

void ObserveLatency(SensorBagLatencyStats* stats, uint64_t latency_ns) noexcept {
  if (stats == nullptr) {
    return;
  }
  ++stats->count;
  stats->total_ns = SaturatingAdd(stats->total_ns, latency_ns);
  stats->max_ns = std::max(stats->max_ns, latency_ns);
  ++stats->buckets[LatencyBucketIndex(latency_ns)];
}


void AppendLatencyJson(std::ostringstream* stream, const char* prefix,
                       const SensorBagLatencyStats& stats) {
  *stream << ",\"" << prefix << "_count\":" << stats.count
          << ",\"" << prefix << "_avg_ns\":" << SensorBagLatencyAverageNs(stats)
          << ",\"" << prefix << "_p50_ns\":"
          << SensorBagLatencyPercentileUpperNs(stats, 50U)
          << ",\"" << prefix << "_p95_ns\":"
          << SensorBagLatencyPercentileUpperNs(stats, 95U)
          << ",\"" << prefix << "_p99_ns\":"
          << SensorBagLatencyPercentileUpperNs(stats, 99U)
          << ",\"" << prefix << "_max_ns\":" << stats.max_ns;
}
void AppendRosbagLatencyJson(std::ostringstream* stream, const char* prefix,
                             const RosbagV2LatencyStats& stats) {
  *stream << ",\"" << prefix << "_count\":" << stats.count
          << ",\"" << prefix << "_avg_ns\":" << RosbagV2LatencyAverageNs(stats)
          << ",\"" << prefix << "_p50_ns\":"
          << RosbagV2LatencyPercentileUpperNs(stats, 50U)
          << ",\"" << prefix << "_p95_ns\":"
          << RosbagV2LatencyPercentileUpperNs(stats, 95U)
          << ",\"" << prefix << "_p99_ns\":"
          << RosbagV2LatencyPercentileUpperNs(stats, 99U)
          << ",\"" << prefix << "_max_ns\":" << stats.max_ns;
}

void AppendRosbagWriterStatsJson(std::ostringstream* stream,
                                 const RosbagV2WriterStats& stats) {
  AppendRosbagLatencyJson(stream, "chunk_open", stats.chunk_open_latency);
  AppendRosbagLatencyJson(stream, "chunk_write", stats.chunk_write_latency);
  AppendRosbagLatencyJson(stream, "chunk_header_patch", stats.chunk_header_patch_latency);
  AppendRosbagLatencyJson(stream, "chunk_index", stats.chunk_index_latency);
  AppendRosbagLatencyJson(stream, "chunk_close", stats.chunk_close_latency);
  AppendRosbagLatencyJson(stream, "record_write", stats.record_write_latency);
  AppendRosbagLatencyJson(stream, "record_header_write", stats.record_header_write_latency);
  AppendRosbagLatencyJson(stream, "record_payload_write", stats.record_payload_write_latency);
  AppendRosbagLatencyJson(stream, "flush_close", stats.flush_close_latency);
  *stream << ",\"raw_write_calls\":" << stats.raw_write_calls
          << ",\"raw_write_bytes\":" << stats.raw_write_bytes
          << ",\"record_header_write_bytes\":" << stats.record_header_write_bytes
          << ",\"record_payload_write_bytes\":" << stats.record_payload_write_bytes;
}



void AppendHeader(std::vector<uint8_t>* payload, uint32_t seq, RosbagTime stamp,
                  const std::string& frame_id) {
  AppendU32(payload, seq);
  AppendRosTime(payload, stamp);
  AppendRosString(payload, frame_id);
}

// sensor_msgs/CompressedImage 的 data 长度在 JPEG 编码完成后回填。
size_t StartCompressedImagePayload(uint32_t seq, RosbagTime stamp,
                                   const std::string& frame_id,
                                   std::vector<uint8_t>* payload) {
  payload->clear();
  payload->reserve(32U + frame_id.size() + kInitialJpegOutputBytes);
  AppendHeader(payload, seq, stamp, frame_id);
  AppendRosString(payload, "jpeg");
  const size_t jpeg_size_offset = payload->size();
  AppendU32(payload, 0U);
  return jpeg_size_offset;
}

void PatchU32(std::vector<uint8_t>* payload, size_t offset, uint32_t value) noexcept {
  std::memcpy(payload->data() + offset, &value, sizeof(value));
}


std::vector<uint8_t> MakeStringPayload(const std::string& value) {
  std::vector<uint8_t> payload;
  AppendRosString(&payload, value);
  return payload;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8U);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void AppendFinishError(SensorBagFinishResult* result, const std::string& error) noexcept {
  if (result == nullptr || error.empty()) {
    return;
  }
  try {
    if (!result->error.empty()) {
      result->error.append("; ");
    }
    result->error.append(error);
  } catch (...) {
  }
}

void MergeAbortResult(SensorBagFinishResult* result,
                      const RosbagV2Writer::AbortResult& abort_result) noexcept {
  if (result == nullptr) {
    return;
  }
  if (!abort_result.cleanup_complete) {
    result->cleanup_complete = false;
    result->outcome = SensorBagFinishOutcome::kCleanupIncomplete;
    AppendFinishError(result, abort_result.error);
  }
}

std::string SessionConfigJson(const Options& options) {
  std::ostringstream stream;
  stream << "{\"schema\":\"robobaton_sensor_bag_v1\","
         << "\"camera_mask\":" << options.camera_mask << ','
         << "\"width\":" << OutputWidth(options) << ','
         << "\"height\":" << OutputHeight(options) << ','
         << "\"camera_fps\":" << options.fps << ','
         << "\"record_frame_skip\":" << options.record_frame_skip << ','
         << "\"image_persistence_fps\":"
         << ImagePersistenceFps(options.fps, options.record_frame_skip) << ','
         << "\"jpeg_quality\":80,"
         << "\"imu_sample_rate_hz\":" << options.imu_sample_rate_hz << ','
         << "\"trigger_mode\":\"" << JsonEscape(options.trigger_mode) << "\","
         << "\"timestamp_domain\":\"" << TimestampDomainName(Sc132OutputTimestampDomain(options))
         << "\","
         << "\"rtsp_path\":\"" << JsonEscape(options.url) << "\"}";
  return stream.str();
}

std::string FrameMetadataJson(const SensorBagFrameJob& job, size_t jpeg_size) {
  std::ostringstream stream;
  stream << "{\"camera_id\":" << job.camera_id << ','
         << "\"sequence\":" << job.sequence << ','
         << "\"frame_id\":" << job.frame_id << ','
         << "\"group_id\":" << job.group_id << ','
         << "\"group_timestamp_ns\":" << job.group_timestamp_ns << ','
         << "\"camera_timestamp_ns\":" << job.camera_timestamp_ns << ','
         << "\"width\":" << job.width << ','
         << "\"height\":" << job.height << ','
         << "\"stride\":" << job.stride << ','
         << "\"vstride\":" << job.vstride << ','
         << "\"jpeg_bytes\":" << jpeg_size << "}";
  return stream.str();
}

}  // namespace
const char* SensorBagFinishOutcomeName(SensorBagFinishOutcome outcome) noexcept {
  switch (outcome) {
    case SensorBagFinishOutcome::kPublishedComplete:
      return "published_complete";
    case SensorBagFinishOutcome::kPublishedPartial:
      return "published_partial";
    case SensorBagFinishOutcome::kAborted:
      return "aborted";
    case SensorBagFinishOutcome::kCleanupIncomplete:
      return "cleanup_incomplete";
    case SensorBagFinishOutcome::kPublishedDurabilityUnproven:
      return "published_durability_unproven";
  }
  return "unknown";
}


uint64_t SensorBagLatencyAverageNs(const SensorBagLatencyStats& stats) noexcept {
  return stats.count == 0U ? 0U : stats.total_ns / stats.count;
}

uint64_t SensorBagLatencyPercentileUpperNs(const SensorBagLatencyStats& stats,
                                           uint32_t percentile) noexcept {
  if (stats.count == 0U) {
    return 0U;
  }
  const uint32_t clamped_percentile = std::max(1U, std::min(percentile, 100U));
  const uint64_t target =
      (stats.count * static_cast<uint64_t>(clamped_percentile) + 99U) / 100U;
  uint64_t cumulative = 0U;
  for (size_t index = 0U; index < stats.buckets.size(); ++index) {
    cumulative += stats.buckets[index];
    if (cumulative >= target) {
      return std::min(stats.max_ns, LatencyBucketUpperBoundNs(index));
    }
  }
  return stats.max_ns;
}

SensorBagRecorder::~SensorBagRecorder() { Abort(); }

void SensorBagRecorder::Start(const Options& options, const std::string& bag_path) {
  if (enabled_) {
    throw std::logic_error("bag recorder already enabled");
  }
  if (options.record_frame_skip > 1U) {
    throw std::invalid_argument("record_frame_skip must be 0 or 1");
  }
  finish_result_ = SensorBagFinishResult{};
  camera_mask_ = options.camera_mask;
  record_target_fps_ = ImagePersistenceFps(options.fps, options.record_frame_skip);
  options_ = options;
  system_clock_ = options.system_clock;
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_ = SensorBagRecorderStats{};
    stats_.frame_queue_capacity = kQueueCapacity;
    stats_.frame_queue_high_watermark = kQueueCapacity;
    stats_.encoded_queue_capacity = kEncodedQueueCapacity;
    stats_.encoded_queue_high_watermark = encoded_admission_high_watermark_;
    stats_.imu_queue_capacity = kImuQueueCapacity;
    stats_.imu_queue_high_watermark = kWriterImuHighWatermark;
  }
  stopping_.store(false, std::memory_order_release);
  finished_ = false;
  fatal_error_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> error_lock(error_mutex_);
    error_message_.clear();
  }
  session_config_written_ = false;
  has_last_data_record_time_ = false;
  last_data_record_time_ns_ = 0U;
  next_frame_order_ = 0U;
  writer_wakeup_generation_ = 0U;
  next_write_order_ = 0U;
  record_selections_.clear();
  next_group_should_record_ = true;
  reusable_jobs_.clear();
  reusable_jobs_.reserve(kReusableJobCapacity);
  reusable_encoded_payloads_.clear();
  reusable_encoded_payloads_.reserve(kReusablePayloadCapacity);
#ifdef RELEASE008_TESTING
  fail_next_payload_pool_recycle_for_test_.store(false, std::memory_order_release);
#endif
  if (encoded_slots_.size() != kEncodedQueueCapacity) {
    encoded_slots_.resize(kEncodedQueueCapacity);
  }
  for (EncodedFrameSlot& slot : encoded_slots_) {
    slot = EncodedFrameSlot{};
  }
  encoded_queue_count_ = 0U;
  encoded_input_closed_ = false;
  imu_queue_.assign(kImuQueueCapacity, icm42688_sample_t{});
  imu_queue_head_ = 0U;
  imu_queue_count_ = 0U;
  frame_queue_at_capacity_ = QueueAtCapacityTracker{};
  encoded_queue_at_capacity_ = QueueAtCapacityTracker{};
  imu_queue_at_capacity_ = QueueAtCapacityTracker{};

  try {
    writer_.Open(bag_path);
    jpeg_encoder_.Start(camera_mask_, OutputWidth(options_), OutputHeight(options_));
    image_connections_.assign(kMaxChannels, 0U);
    camera_info_connections_.assign(kMaxChannels, 0U);
    frame_metadata_connections_.assign(kMaxChannels, 0U);
    camera_info_written_.assign(kMaxChannels, false);

    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(camera_mask_, camera_id)) {
        continue;
      }
      const std::string prefix = "/camera" + std::to_string(camera_id);
      image_connections_[camera_id] = writer_.AddConnection(
          prefix + "/image/compressed", "sensor_msgs/CompressedImage",
          "8f7a12909da2c9d3332d540a0977563f", kCompressedImageDefinition);
      camera_info_connections_[camera_id] = writer_.AddConnection(
          prefix + "/camera_info", "sensor_msgs/CameraInfo",
          "c9a58c1b0b154e0e6da7578cb991d214", kCameraInfoDefinition);
      frame_metadata_connections_[camera_id] = writer_.AddConnection(
          prefix + "/frame_metadata", "std_msgs/String",
          "992ce8a1687cec8c8bd883ec73ca41d1", kStringDefinition);
    }
    imu_connection_ = writer_.AddConnection("/imu/data", "sensor_msgs/Imu",
                                            "6a62c6daae103f4ff57a132d6f95cec2",
                                            kImuDefinition);
    session_config_connection_ = writer_.AddConnection(
        "/robobaton/session_config", "std_msgs/String",
        "992ce8a1687cec8c8bd883ec73ca41d1", kStringDefinition);
    session_status_connection_ = writer_.AddConnection(
        "/robobaton/session_status", "std_msgs/String",
        "992ce8a1687cec8c8bd883ec73ca41d1", kStringDefinition);

    enabled_ = true;
    writer_worker_ = std::thread(&SensorBagRecorder::WriterEntry, this);
    workers_.reserve(kFrameWorkerCount);
    for (size_t index = 0U; index < kFrameWorkerCount; ++index) {
      workers_.emplace_back(&SensorBagRecorder::WorkerEntry, this, static_cast<int>(index));
    }
  } catch (...) {
    stopping_.store(true, std::memory_order_release);
    queue_condition_.notify_all();
    imu_queue_condition_.notify_all();
    StopWorkers();
    writer_.Abort();
    jpeg_encoder_.Stop();
    enabled_ = false;
    finished_ = false;
    throw;
  }
}

void SensorBagRecorder::ObserveFrame(const QueuedFrame& frame) {
  if (!enabled_) {
    return;
  }
  if (HasFatalError()) {
    throw std::runtime_error(ErrorMessage());
  }
  RecordSourceFrameSetSeen();
  if (!ShouldRecordGroup(frame.group_id, frame.group_timestamp_ns)) {
    return;
  }
  RecordSelectedGroup();
  if (frame.y_data == nullptr || frame.uv_data == nullptr || frame.width == 0U ||
      frame.height == 0U || frame.stride < frame.width || frame.vstride < frame.height) {
    throw std::runtime_error("invalid frame for bag recorder");
  }
  if (EncodedBackpressureActive()) {
    // encoded backlog 高水位只丢当前保存帧，不反压采集/RTSP 回调线程。
    RecordEncodedBackpressureReject();
    return;
  }

  SensorBagFrameJob job;
  if (frame.frame != nullptr) {
    if (sc132_frame_retain(frame.frame) != SC132_STATUS_OK) {
      SetFatalError("bag recorder frame retain failed");
      throw std::runtime_error("bag recorder frame retain failed");
    }
    job.raw_frame.frame = frame.frame;
  }
  job.camera_id = frame.channel;
  job.sequence = frame.sequence;
  job.frame_id = frame.frame_id;
  job.group_id = frame.group_id;
  job.group_timestamp_ns = frame.group_timestamp_ns;
  job.camera_timestamp_ns = frame.camera_timestamp_ns;
  job.width = frame.width;
  job.height = frame.height;
  job.stride = frame.stride;
  job.vstride = frame.vstride;
  job.raw_frame.channel = frame.channel;
  job.raw_frame.sequence = frame.sequence;
  job.raw_frame.frame_id = frame.frame_id;
  job.raw_frame.group_id = frame.group_id;
  job.raw_frame.group_timestamp_ns = frame.group_timestamp_ns;
  job.raw_frame.group_max_skew_ns = frame.group_max_skew_ns;
  job.raw_frame.camera_timestamp_ns = frame.camera_timestamp_ns;
  job.raw_frame.rtsp_timestamp_ns = frame.rtsp_timestamp_ns;
  job.raw_frame.group_timestamp_domain = frame.group_timestamp_domain;
  job.raw_frame.camera_timestamp_domain = frame.camera_timestamp_domain;
  job.raw_frame.rtsp_timestamp_domain = frame.rtsp_timestamp_domain;
  job.raw_frame.enqueue_timestamp_ns = frame.enqueue_timestamp_ns;
  job.raw_frame.y_data = frame.y_data;
  job.raw_frame.uv_data = frame.uv_data;
  job.raw_frame.y_phys = frame.y_phys;
  job.raw_frame.uv_phys = frame.uv_phys;
  job.raw_frame.y_size = frame.y_size;
  job.raw_frame.uv_size = frame.uv_size;
  job.raw_frame.width = frame.width;
  job.raw_frame.height = frame.height;
  job.raw_frame.stride = frame.stride;
  job.raw_frame.vstride = frame.vstride;

  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (stopping_.load(std::memory_order_acquire)) {
    job.raw_frame.Reset();
    return;
  }
  if (queue_.size() >= kQueueCapacity) {
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      ++stats_.frame_queue_full_rejects;
    }
    job.raw_frame.Reset();
    return;
  }
  try {
    job.record_order = next_frame_order_;
    queue_.push_back(std::move(job));
    ++next_frame_order_;
  } catch (...) {
    job.raw_frame.Reset();
    SetFatalError("bag recorder frame queue allocation failed");
    throw;
  }
  UpdateFrameQueueAccountingLocked(queue_.size());
  RecordAdmittedGroup();
  lock.unlock();
  queue_condition_.notify_all();
}

SensorBagRecorder::FrameSetAdmissionStatus SensorBagRecorder::TryAcceptFrameSet(
    const sc132_frame_set_t& frame_set) noexcept {
  try {
    if (!enabled_) {
      return FrameSetAdmissionStatus::kSkipped;
    }
    if (HasFatalError()) {
      return FrameSetAdmissionStatus::kFatal;
    }
    const uint32_t expected_count =
        static_cast<uint32_t>(CameraMaskPopCount(camera_mask_));
    if (frame_set.struct_size != sizeof(frame_set) || frame_set.camera_count == 0U ||
        frame_set.camera_count != expected_count ||
        frame_set.camera_count > SC132_FRAME_SET_MAX_CAMERAS) {
      SetFatalError("invalid frame set for bag recorder");
      return FrameSetAdmissionStatus::kFatal;
    }
    RecordSourceFrameSetSeen();
    if (!ShouldRecordGroup(frame_set.group_id, frame_set.group_timestamp_ns)) {
      return FrameSetAdmissionStatus::kSkipped;
    }
    RecordSelectedGroup();

    std::array<SensorBagFrameJob, kMaxChannels> jobs{};
    std::array<sc132_frame_t*, kMaxChannels> frame_handles{};
    uint32_t observed_mask = 0U;
    for (uint32_t index = 0U; index < frame_set.camera_count; ++index) {
      const sc132_frame_set_item_t& item = frame_set.items[index];
      if (item.camera_id >= static_cast<uint32_t>(kMaxChannels) || item.frame == nullptr ||
          !CameraMaskContains(camera_mask_, static_cast<int>(item.camera_id))) {
        SetFatalError("invalid frame set item for bag recorder");
        return FrameSetAdmissionStatus::kFatal;
      }
      const uint32_t bit = 1U << item.camera_id;
      if ((observed_mask & bit) != 0U) {
        SetFatalError("duplicate frame set camera for bag recorder");
        return FrameSetAdmissionStatus::kFatal;
      }
      observed_mask |= bit;

      sc132_frame_info_t info{};
      info.struct_size = sizeof(info);
      if (sc132_frame_get_info(item.frame, &info) != SC132_STATUS_OK ||
          info.struct_size != sizeof(info) || info.camera_id != item.camera_id ||
          info.sequence != item.sequence || info.frame_id != item.frame_id ||
          info.width != item.width || info.height != item.height || info.y_data == nullptr ||
          info.uv_data == nullptr || info.width == 0U || info.height == 0U ||
          info.stride < info.width || info.vstride < info.height) {
        SetFatalError("frame set info mismatch for bag recorder");
        return FrameSetAdmissionStatus::kFatal;
      }

      SensorBagFrameJob& job = jobs[index];
      job.camera_id = static_cast<int>(item.camera_id);
      job.sequence = info.sequence;
      job.frame_id = info.frame_id;
      job.group_id = frame_set.group_id;
      job.group_timestamp_ns = frame_set.group_timestamp_ns;
      job.camera_timestamp_ns = item.timestamp_ns;
      job.width = info.width;
      job.height = info.height;
      job.stride = info.stride;
      job.vstride = info.vstride;

      QueuedFrame& raw = job.raw_frame;
      raw.channel = job.camera_id;
      raw.sequence = job.sequence;
      raw.frame_id = job.frame_id;
      raw.group_id = job.group_id;
      raw.group_timestamp_ns = job.group_timestamp_ns;
      raw.group_max_skew_ns = frame_set.max_skew_ns;
      raw.camera_timestamp_ns = item.timestamp_ns;
      raw.rtsp_timestamp_ns = frame_set.group_timestamp_ns;
      raw.group_timestamp_domain = Sc132OutputTimestampDomain(options_);
      raw.camera_timestamp_domain = raw.group_timestamp_domain;
      raw.rtsp_timestamp_domain = raw.group_timestamp_domain;
      raw.enqueue_timestamp_ns = SteadyClockNowNs();
      raw.y_data = info.y_data;
      raw.uv_data = info.uv_data;
      raw.y_phys = info.y_phys;
      raw.uv_phys = info.uv_phys;
      raw.y_size = info.y_size;
      raw.uv_size = info.uv_size;
      raw.width = info.width;
      raw.height = info.height;
      raw.stride = info.stride;
      raw.vstride = info.vstride;
      frame_handles[index] = item.frame;
    }
    if (observed_mask != camera_mask_) {
      SetFatalError("frame set mask mismatch for bag recorder");
      return FrameSetAdmissionStatus::kFatal;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      return FrameSetAdmissionStatus::kStopped;
    }
    if (queue_.size() + frame_set.camera_count > kQueueCapacity) {
      {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.frame_queue_full_rejects;
      }
      return FrameSetAdmissionStatus::kQueueFull;
    }
    size_t retained_count = 0U;
    for (; retained_count < frame_set.camera_count; ++retained_count) {
      if (sc132_frame_retain(frame_handles[retained_count]) != SC132_STATUS_OK) {
        for (size_t release_index = 0U; release_index < retained_count; ++release_index) {
          sc132_frame_release(frame_handles[release_index]);
        }
        SetFatalError("bag recorder frame retain failed");
        return FrameSetAdmissionStatus::kFatal;
      }
    }

    for (uint32_t index = 0U; index < frame_set.camera_count; ++index) {
      jobs[index].raw_frame.frame = frame_handles[index];
    }
    const size_t original_size = queue_.size();
    const auto rollback_staged_jobs = [&]() noexcept {
      while (queue_.size() > original_size) {
        SensorBagFrameJob rejected = std::move(queue_.back());
        queue_.pop_back();
        rejected.raw_frame.Reset();
      }
    };
    try {
      for (uint32_t index = 0U; index < frame_set.camera_count; ++index) {
        jobs[index].record_order = next_frame_order_ + index;
        queue_.push_back(std::move(jobs[index]));
      }
    } catch (...) {
      rollback_staged_jobs();
      for (uint32_t index = 0U; index < frame_set.camera_count; ++index) {
        jobs[index].raw_frame.Reset();
      }
      SetFatalError("bag recorder frame queue allocation failed");
      return FrameSetAdmissionStatus::kFatal;
    }

    // The queue mutex keeps staged jobs invisible to workers. Commit old-group eviction
    // only after all incoming retains and queue allocations have succeeded.
    if (!MakeEncodedAdmissionRoomByEvictingOldestReadyFrameSets()) {
      rollback_staged_jobs();
      RecordEncodedBackpressureReject();
      return FrameSetAdmissionStatus::kQueueFull;
    }

    next_frame_order_ += frame_set.camera_count;
    UpdateFrameQueueAccountingLocked(queue_.size());
    RecordAdmittedGroup();
    lock.unlock();
    queue_condition_.notify_all();
    return FrameSetAdmissionStatus::kAccepted;
  } catch (const std::exception& error) {
    SetFatalError(error.what());
    return FrameSetAdmissionStatus::kFatal;
  } catch (...) {
    SetFatalError("unknown frame set admission failure");
    return FrameSetAdmissionStatus::kFatal;
  }
}

void SensorBagRecorder::ObserveFrameSet(const sc132_frame_set_t& frame_set) {
  const FrameSetAdmissionStatus status = TryAcceptFrameSet(frame_set);
  if (status == FrameSetAdmissionStatus::kFatal) {
    throw std::runtime_error(ErrorMessage());
  }
}

void SensorBagRecorder::ObserveImu(const icm42688_sample_t& sample) {
  if (!enabled_) {
    return;
  }
  if (HasFatalError()) {
    throw std::runtime_error(ErrorMessage());
  }

  if (sample.struct_size != sizeof(sample)) {
    SetFatalError("invalid IMU sample for bag recorder");
    throw std::runtime_error("invalid IMU sample for bag recorder");
  }

  bool should_notify_writer = false;
  {
    std::lock_guard<std::mutex> lock(imu_queue_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      throw std::runtime_error("bag recorder is stopping");
    }
    if (imu_queue_count_ >= kImuQueueCapacity) {
      {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.imu_queue_full_rejects;
      }
      return;
    }
    const bool was_empty = imu_queue_count_ == 0U;
    const size_t tail = (imu_queue_head_ + imu_queue_count_) % kImuQueueCapacity;
    imu_queue_[tail] = sample;
    ++imu_queue_count_;
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      ++stats_.recorder_imu_admitted;
    }
    UpdateImuQueueAccountingLocked(imu_queue_count_);
    should_notify_writer = was_empty;
  }
  if (should_notify_writer) {
    NotifyWriterOne(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
  }
}

SensorBagFinishResult SensorBagRecorder::Finish(bool session_success) noexcept {
  if (!enabled_ || finished_) {
    return finish_result_;
  }
  finish_result_ = SensorBagFinishResult{};
  stopping_.store(true, std::memory_order_release);
  jpeg_encoder_.NotifySlotWaiters();
  queue_condition_.notify_all();
  imu_queue_condition_.notify_all();
  StopWorkers();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (SensorBagFrameJob& job : queue_) {
      jpeg_encoder_.ReleaseSlot(job.jpeg_slot);
      job.jpeg_slot = nullptr;
    }
    queue_.clear();
  }

  const bool data_complete = session_success && !HasFatalError() && !HasRecordedDrops();
  finish_result_.data_complete = data_complete;
  finish_result_.session_uuid = writer_.session_uuid();
  std::string cleanup_error;
  if (!jpeg_encoder_.Stop(&cleanup_error)) {
    SetFatalError(cleanup_error.empty() ? "hardware JPEG cleanup failed" : cleanup_error);
    finish_result_.outcome = SensorBagFinishOutcome::kCleanupIncomplete;
    finish_result_.cleanup_complete = false;
    AppendFinishError(&finish_result_, ErrorMessage());
    MergeAbortResult(&finish_result_, writer_.Abort());
    finished_ = true;
    enabled_ = false;
    return finish_result_;
  }
  if (HasFatalError()) {
    finish_result_.outcome = SensorBagFinishOutcome::kAborted;
    AppendFinishError(&finish_result_, ErrorMessage());
    MergeAbortResult(&finish_result_, writer_.Abort());
    finished_ = true;
    enabled_ = false;
    return finish_result_;
  }

  try {
    const SensorBagFinishOutcome planned_outcome =
        data_complete ? SensorBagFinishOutcome::kPublishedComplete
                      : SensorBagFinishOutcome::kPublishedPartial;
    const std::string planned_path = data_complete ? writer_.final_path() : writer_.partial_path();
    WriteSessionStatus(planned_outcome, data_complete, planned_path, writer_.session_uuid());
    const RosbagV2Writer::PublishResult publish_result = writer_.Finish(data_complete);
    finish_result_.data_complete = publish_result.data_complete;
    finish_result_.cleanup_complete = publish_result.cleanup_complete;
    finish_result_.session_uuid = publish_result.session_uuid;
    finish_result_.published_path = publish_result.published_path;
    finish_result_.quarantine_path = publish_result.quarantine_path;
    AppendFinishError(&finish_result_, publish_result.error);
    if (!publish_result.cleanup_complete) {
      finish_result_.outcome = SensorBagFinishOutcome::kCleanupIncomplete;
    } else if (publish_result.outcome == RosbagV2Writer::PublishOutcome::kPublishedDurabilityUnproven) {
      finish_result_.outcome = SensorBagFinishOutcome::kPublishedDurabilityUnproven;
    } else {
      finish_result_.outcome = planned_outcome;
    }
  } catch (const std::exception& error) {
    SetFatalError(error.what());
    finish_result_.outcome = SensorBagFinishOutcome::kAborted;
    AppendFinishError(&finish_result_, error.what());
    MergeAbortResult(&finish_result_, writer_.Abort());
  } catch (...) {
    SetFatalError("unknown recorder finish failure");
    finish_result_.outcome = SensorBagFinishOutcome::kAborted;
    AppendFinishError(&finish_result_, ErrorMessage());
    MergeAbortResult(&finish_result_, writer_.Abort());
  }
  finished_ = true;
  enabled_ = false;
  return finish_result_;
}

void SensorBagRecorder::Abort() noexcept {
  stopping_.store(true, std::memory_order_release);
  jpeg_encoder_.NotifySlotWaiters();
  queue_condition_.notify_all();
  imu_queue_condition_.notify_all();
  StopWorkers();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (SensorBagFrameJob& job : queue_) {
      jpeg_encoder_.ReleaseSlot(job.jpeg_slot);
      job.jpeg_slot = nullptr;
    }
    queue_.clear();
  }
  const RosbagV2Writer::AbortResult abort_result = writer_.Abort();
  finish_result_.outcome = SensorBagFinishOutcome::kAborted;
  finish_result_.cleanup_complete = abort_result.cleanup_complete;
  finish_result_.session_uuid = writer_.session_uuid();
  AppendFinishError(&finish_result_, abort_result.error);
  if (!abort_result.cleanup_complete) {
    SetFatalError(abort_result.error.empty() ? "bag staging cleanup failed" : abort_result.error);
    finish_result_.outcome = SensorBagFinishOutcome::kCleanupIncomplete;
  }
  std::string cleanup_error;
  if (!jpeg_encoder_.Stop(&cleanup_error)) {
    SetFatalError(cleanup_error.empty() ? "hardware JPEG cleanup failed" : cleanup_error);
  }
  enabled_ = false;
}

bool SensorBagRecorder::HasFatalError() const noexcept {
  return fatal_error_.load(std::memory_order_acquire);
}

SensorBagRecorderStats SensorBagRecorder::SnapshotStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  SensorBagRecorderStats snapshot = stats_;
  const uint64_t admission_dropped_groups =
      snapshot.recorder_selected_groups >= snapshot.recorder_admitted_groups
          ? snapshot.recorder_selected_groups - snapshot.recorder_admitted_groups
          : 0U;
  snapshot.recorder_dropped_groups =
      SaturatingAdd(admission_dropped_groups, snapshot.encoded_queue_oldest_evicted_groups);
  snapshot.writer_stats = writer_.SnapshotStats();
  return snapshot;
}

#ifdef RELEASE008_TESTING
void SensorBagRecorder::PauseWriterForTest() noexcept {
  writer_paused_for_test_.store(true, std::memory_order_release);
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
}

void SensorBagRecorder::ResumeWriterForTest() noexcept {
  writer_paused_for_test_.store(false, std::memory_order_release);
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
}

uint64_t SensorBagRecorder::WriterWakeupGenerationForTest() noexcept {
  std::lock_guard<std::mutex> lock(writer_wakeup_mutex_);
  return writer_wakeup_generation_;
}

void SensorBagRecorder::SetEncodedAdmissionHighWatermarkForTest(size_t high_watermark) noexcept {
  std::lock_guard<std::mutex> lock(encoded_mutex_);
  if (high_watermark == 0U || high_watermark > kEncodedQueueCapacity) {
    encoded_admission_high_watermark_ = kEncodedQueueCapacity;
    return;
  }
  encoded_admission_high_watermark_ = high_watermark;
}

void SensorBagRecorder::FailNextPayloadPoolRecycleForTest() noexcept {
  fail_next_payload_pool_recycle_for_test_.store(true, std::memory_order_release);
}

#endif

bool SensorBagRecorder::EncodedBackpressureActive() noexcept {
  std::lock_guard<std::mutex> lock(encoded_mutex_);
  const size_t high_watermark = std::min(encoded_admission_high_watermark_,
                                        kEncodedQueueCapacity);
  return encoded_queue_count_ >= high_watermark;
}

bool SensorBagRecorder::MakeEncodedAdmissionRoomByEvictingOldestReadyFrameSets() noexcept {
  std::array<EncodedFrameJob,
             kMaxChannels * kMaxOldestFrameSetsEvictedPerAdmission>
      evicted_jobs{};
  size_t evicted_job_count = 0U;
  uint64_t evicted_group_count = 0U;
  uint64_t evicted_frame_count = 0U;
  uint64_t evicted_bytes = 0U;
  bool admission_room_available = false;

  {
    std::lock_guard<std::mutex> lock(encoded_mutex_);
    const size_t high_watermark = std::min(encoded_admission_high_watermark_,
                                          kEncodedQueueCapacity);
    if (encoded_queue_count_ < high_watermark) {
      return true;
    }

    const size_t group_size = static_cast<size_t>(CameraMaskPopCount(camera_mask_));
    if (group_size == 0U || group_size > kMaxChannels) {
      return false;
    }

    // next_write_order_ advances when the writer claims a frame. If it is inside a group,
    // the remainder belongs to an already-claimed frame-set and must stay protected.
    uint64_t candidate_order = next_write_order_;
    const uint64_t group_remainder = candidate_order % group_size;
    if (group_remainder != 0U) {
      candidate_order += group_size - group_remainder;
    }

    const size_t frames_to_free = encoded_queue_count_ - high_watermark + 1U;
    const size_t groups_to_evict =
        (frames_to_free + group_size - 1U) / group_size;
    if (groups_to_evict == 0U ||
        groups_to_evict > kMaxOldestFrameSetsEvictedPerAdmission) {
      return false;
    }

    // Validate the complete replacement set before moving any payload. If all required
    // frame-sets are not ready, leave every old frame intact and reject only the newcomer.
    for (size_t group_index = 0U; group_index < groups_to_evict; ++group_index) {
      const uint64_t group_order = candidate_order + group_index * group_size;
      bool complete_ready_group = true;
      uint64_t group_id = 0U;
      for (size_t frame_index = 0U; frame_index < group_size; ++frame_index) {
        const uint64_t record_order = group_order + frame_index;
        const EncodedFrameSlot& slot =
            encoded_slots_[record_order % kEncodedQueueCapacity];
        if (!slot.occupied || slot.evicted || slot.record_order != record_order) {
          complete_ready_group = false;
          break;
        }
        if (frame_index == 0U) {
          group_id = slot.job.job.group_id;
        } else if (slot.job.job.group_id != group_id) {
          complete_ready_group = false;
          break;
        }
      }
      if (!complete_ready_group) {
        return false;
      }
    }

    for (size_t group_index = 0U; group_index < groups_to_evict; ++group_index) {
      const uint64_t group_order = candidate_order + group_index * group_size;
      for (size_t frame_index = 0U; frame_index < group_size; ++frame_index) {
        const uint64_t record_order = group_order + frame_index;
        EncodedFrameSlot& slot = encoded_slots_[record_order % kEncodedQueueCapacity];
        evicted_bytes = SaturatingAdd(
            evicted_bytes,
            SaturatingAdd(static_cast<uint64_t>(slot.job.image_payload.size()),
                          static_cast<uint64_t>(slot.job.metadata_payload.size())));
        evicted_jobs[evicted_job_count++] = std::move(slot.job);
        slot.job = EncodedFrameJob{};
        slot.record_order = record_order;
        slot.occupied = true;
        slot.evicted = true;
        --encoded_queue_count_;
      }
      ++evicted_group_count;
      evicted_frame_count = SaturatingAdd(evicted_frame_count, group_size);
    }

    admission_room_available = encoded_queue_count_ < high_watermark;
    if (evicted_group_count != 0U) {
      UpdateEncodedQueueAccountingLocked(encoded_queue_count_);
    }
  }

  if (evicted_group_count != 0U) {
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      stats_.encoded_queue_oldest_evicted_groups = SaturatingAdd(
          stats_.encoded_queue_oldest_evicted_groups, evicted_group_count);
      stats_.encoded_queue_oldest_evicted_frames = SaturatingAdd(
          stats_.encoded_queue_oldest_evicted_frames, evicted_frame_count);
      stats_.encoded_queue_oldest_evicted_bytes = SaturatingAdd(
          stats_.encoded_queue_oldest_evicted_bytes, evicted_bytes);
    }
    for (size_t index = 0U; index < evicted_job_count; ++index) {
      ReleaseEncodedFrameJob(&evicted_jobs[index]);
    }
    encoded_condition_.notify_all();
    NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
  }
  return admission_room_available;
}

void SensorBagRecorder::RecordEncodedBackpressureReject() noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  ++stats_.encoded_queue_full_rejects;
}

void SensorBagRecorder::RecordSourceFrameSetSeen() noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  ++stats_.source_frame_sets_seen;
}

void SensorBagRecorder::RecordSelectedGroup() noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  ++stats_.recorder_selected_groups;
}

void SensorBagRecorder::RecordAdmittedGroup() noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  ++stats_.recorder_admitted_groups;
}

void SensorBagRecorder::UpdateWrittenGroupCountLocked() noexcept {
  uint64_t min_count = std::numeric_limits<uint64_t>::max();
  bool any_camera = false;
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (!CameraMaskContains(camera_mask_, camera_id)) {
      continue;
    }
    any_camera = true;
    min_count = std::min(min_count, stats_.image_frames_by_camera[static_cast<size_t>(camera_id)]);
  }
  stats_.recorder_written_groups = any_camera ? min_count : 0U;
}

void SensorBagRecorder::UpdateQueueAtCapacity(bool at_capacity,
                                              QueueAtCapacityTracker* tracker,
                                              uint64_t* dwell_ns,
                                              uint64_t* events) noexcept {
  if (tracker == nullptr || dwell_ns == nullptr || events == nullptr) {
    return;
  }
  const uint64_t now_ns = SteadyClockNowNs();
  if (at_capacity && !tracker->active) {
    tracker->active = true;
    tracker->start_ns = now_ns;
    *events = SaturatingAdd(*events, 1U);
    return;
  }
  if (!at_capacity && tracker->active) {
    *dwell_ns = SaturatingAdd(*dwell_ns, now_ns - tracker->start_ns);
    tracker->active = false;
    tracker->start_ns = 0U;
  }
}

void SensorBagRecorder::UpdateFrameQueueAccountingLocked(size_t depth) noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  stats_.frame_queue_peak_depth =
      std::max(stats_.frame_queue_peak_depth, static_cast<uint64_t>(depth));
  UpdateQueueAtCapacity(depth >= kQueueCapacity, &frame_queue_at_capacity_,
                        &stats_.frame_queue_at_capacity_dwell_ns,
                        &stats_.frame_queue_at_capacity_events);
}

void SensorBagRecorder::UpdateEncodedQueueAccountingLocked(size_t depth) noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  stats_.encoded_queue_peak_depth =
      std::max(stats_.encoded_queue_peak_depth, static_cast<uint64_t>(depth));
  stats_.encoded_queue_high_watermark = encoded_admission_high_watermark_;
  UpdateQueueAtCapacity(depth >= encoded_admission_high_watermark_,
                        &encoded_queue_at_capacity_,
                        &stats_.encoded_queue_at_capacity_dwell_ns,
                        &stats_.encoded_queue_at_capacity_events);
}

void SensorBagRecorder::UpdateImuQueueAccountingLocked(size_t depth) noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  stats_.imu_queue_peak_depth =
      std::max(stats_.imu_queue_peak_depth, static_cast<uint64_t>(depth));
  UpdateQueueAtCapacity(depth >= kImuQueueCapacity, &imu_queue_at_capacity_,
                        &stats_.imu_queue_at_capacity_dwell_ns,
                        &stats_.imu_queue_at_capacity_events);
}

bool SensorBagRecorder::HasRecordedDrops() const noexcept {
  std::lock_guard<std::mutex> stats_lock(stats_mutex_);
  return stats_.frame_queue_full_rejects != 0U ||
         stats_.encoded_queue_full_rejects != 0U ||
         stats_.encoded_queue_oldest_evicted_groups != 0U ||
         stats_.imu_queue_full_rejects != 0U;
}

void SensorBagRecorder::ObserveCopyTiming(
    uint64_t copy_ns, const X5JpegNv12CopyResult& copy_result) noexcept {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_.nv12_copy_bytes = SaturatingAdd(
      stats_.nv12_copy_bytes,
      SaturatingAdd(copy_result.bulk_bytes, copy_result.row_bytes));
  stats_.nv12_copy_bulk_plane_count = SaturatingAdd(
      stats_.nv12_copy_bulk_plane_count, copy_result.bulk_plane_count);
  stats_.nv12_copy_bulk_bytes = SaturatingAdd(stats_.nv12_copy_bulk_bytes,
                                             copy_result.bulk_bytes);
  stats_.nv12_copy_row_count = SaturatingAdd(stats_.nv12_copy_row_count,
                                            copy_result.row_copy_count);
  stats_.nv12_copy_row_bytes = SaturatingAdd(stats_.nv12_copy_row_bytes,
                                            copy_result.row_bytes);
  ObserveLatency(&stats_.nv12_copy_latency, copy_ns);
}

void SensorBagRecorder::ObserveJpegTiming(uint64_t encode_ns) noexcept {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ObserveLatency(&stats_.jpeg_encode_latency, encode_ns);
}

void SensorBagRecorder::ObserveWriterTiming(uint64_t order_wait_ns,
                                            uint64_t writer_wait_ns,
                                            uint64_t writer_hold_ns,
                                            bool image_write) noexcept {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_.write_order_wait_max_ns = std::max(stats_.write_order_wait_max_ns, order_wait_ns);
  stats_.writer_mutex_wait_max_ns = std::max(stats_.writer_mutex_wait_max_ns, writer_wait_ns);
  stats_.writer_mutex_hold_max_ns = std::max(stats_.writer_mutex_hold_max_ns, writer_hold_ns);
  if (image_write) {
    ObserveLatency(&stats_.write_order_wait_latency, order_wait_ns);
    ObserveLatency(&stats_.image_writer_wait_latency, writer_wait_ns);
    ObserveLatency(&stats_.image_writer_hold_latency, writer_hold_ns);
  } else {
    ObserveLatency(&stats_.imu_writer_wait_latency, writer_wait_ns);
    ObserveLatency(&stats_.imu_writer_hold_latency, writer_hold_ns);
  }
}

void SensorBagRecorder::WorkerEntry(int camera_id) noexcept {
  ApplyRecorderWorkerPriority(kRecorderEncodeWorkerNice);
  while (!HasFatalError()) {
    SensorBagFrameJob job;
    if (!PopFrame(camera_id, &job)) {
      break;
    }
    if (HasFatalError()) {
      ReleaseFrameJob(&job);
      break;
    }
    bool job_recycled = false;
    try {
      EncodedFrameJob encoded = EncodeFrame(&job);
      ReleaseFrameJob(&job);
      job_recycled = true;
      PublishEncodedFrame(std::move(encoded));
    } catch (const std::exception& error) {
      if (!job_recycled) {
        if (job.jpeg_slot != nullptr && job.jpeg_slot->submitted_to_hardware) {
          jpeg_encoder_.QuarantineSlot(job.jpeg_slot);
          job.jpeg_slot = nullptr;
        } else {
          ReleaseFrameJob(&job);
        }
      }
      SetFatalError(error.what());
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
      break;
    } catch (...) {
      if (!job_recycled) {
        if (job.jpeg_slot != nullptr && job.jpeg_slot->submitted_to_hardware) {
          jpeg_encoder_.QuarantineSlot(job.jpeg_slot);
          job.jpeg_slot = nullptr;
        } else {
          ReleaseFrameJob(&job);
        }
      }
      SetFatalError("unknown frame encode failure");
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
      break;
    }
  }
}

void SensorBagRecorder::WriterEntry() noexcept {
  ApplyRecorderWorkerPriority(kRecorderWriterNice);
  std::vector<uint8_t> imu_payload;
  imu_payload.reserve(320U);
  std::array<icm42688_sample_t, kWriterImuHighWatermarkDrainBatch> imu_batch{};
  std::vector<PendingEncodedWrite> pending_images;
  pending_images.reserve(kWriterImagePrefetchLimit);
  size_t pending_image_head = 0U;
  size_t image_burst_count = 0U;

  const auto pending_image_count = [&]() -> size_t {
    return pending_images.size() - pending_image_head;
  };
  const auto compact_pending_images = [&]() {
    if (pending_image_head == 0U) {
      return;
    }
    if (pending_image_head == pending_images.size()) {
      pending_images.clear();
      pending_image_head = 0U;
      return;
    }
    if (pending_image_head >= kWriterImagePrefetchLimit / 2U) {
      pending_images.erase(pending_images.begin(),
                           pending_images.begin() +
                               static_cast<std::ptrdiff_t>(pending_image_head));
      pending_image_head = 0U;
    }
  };
  const auto prefetch_ready_images = [&]() {
    compact_pending_images();
    while (pending_image_count() < kWriterImagePrefetchLimit) {
      EncodedFrameJob encoded;
      uint64_t order_wait_ns = 0U;
      if (!PopNextEncodedFrame(&encoded, &order_wait_ns)) {
        break;
      }
      pending_images.push_back(PendingEncodedWrite{std::move(encoded), order_wait_ns});
      const uint64_t backlog_depth = static_cast<uint64_t>(pending_image_count());
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      stats_.writer_image_backlog_peak_depth =
          std::max(stats_.writer_image_backlog_peak_depth, backlog_depth);
    }
  };
  const auto write_pending_image = [&]() -> bool {
    PendingEncodedWrite pending = std::move(pending_images[pending_image_head]);
    ++pending_image_head;
    try {
      WriteEncodedFrameToBag(pending.encoded, pending.order_wait_ns);
      ReleaseEncodedFrameJob(&pending.encoded);
      ++image_burst_count;
      return true;
    } catch (const std::exception& error) {
      SetFatalError(error.what());
      return false;
    } catch (...) {
      SetFatalError("unknown encoded frame write failure");
      return false;
    }
  };
  const auto write_imu_sample = [&](const icm42688_sample_t& sample) -> bool {
    try {
      WriteImuSample(sample, &imu_payload);
      image_burst_count = 0U;
      return true;
    } catch (const std::exception& error) {
      SetFatalError(error.what());
      return false;
    } catch (...) {
      SetFatalError("unknown IMU bag recorder failure");
      return false;
    }
  };
  const auto drain_imu_samples = [&](size_t limit) -> bool {
    size_t drained = 0U;
    while (drained < limit) {
      const size_t batch_limit = std::min(limit - drained, imu_batch.size());
      const size_t batch_count = PopImuBatch(imu_batch.data(), batch_limit);
      if (batch_count == 0U) {
        return true;
      }
      for (size_t index = 0U; index < batch_count; ++index) {
        if (!write_imu_sample(imu_batch[index])) {
          return false;
        }
      }
      drained += batch_count;
    }
    return true;
  };
  const auto imu_queue_depth = [&]() -> size_t {
    std::lock_guard<std::mutex> lock(imu_queue_mutex_);
    return imu_queue_count_;
  };
  while (!HasFatalError()) {
    if (pending_image_count() == 0U) {
      {
        std::unique_lock<std::mutex> wake_lock(writer_wakeup_mutex_);
        const uint64_t observed_generation = writer_wakeup_generation_;
        writer_condition_.wait(wake_lock, [&] {
          return writer_wakeup_generation_ != observed_generation || WriterInputReady();
        });
      }
      if (HasFatalError()) {
        break;
      }
    }

#ifdef RELEASE008_TESTING
    if (writer_paused_for_test_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> wake_lock(writer_wakeup_mutex_);
      writer_condition_.wait(wake_lock, [&] {
        return HasFatalError() ||
               !writer_paused_for_test_.load(std::memory_order_acquire);
      });
      if (HasFatalError()) {
        break;
      }
    }
#endif

    prefetch_ready_images();

    if (pending_image_count() > 0U) {
      const bool imu_high_water = imu_queue_depth() >= kWriterImuHighWatermark;
      if (imu_high_water || image_burst_count >= kWriterImageBurstBeforeImu) {
        // 高水位 IMU backlog 抢占 image burst，避免秒级写盘停顿后继续把 IMU ring 顶满。
        const size_t drain_limit = imu_high_water ? kWriterImuHighWatermarkDrainBatch
                                                  : kWriterImuDrainBatch;
        if (!drain_imu_samples(drain_limit)) {
          break;
        }
      }
      if (!write_pending_image()) {
        break;
      }
      continue;
    }

    if (!drain_imu_samples(kWriterImuDrainBatch)) {
      break;
    }
    if (WriterInputsClosed()) {
      break;
    }
    if (HasClosedEncodedOrderGap()) {
      SetFatalError("bag recorder encoded order gap");
      break;
    }
  }
}


void SensorBagRecorder::StopWorkers() noexcept {
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> lock(encoded_mutex_);
    encoded_input_closed_ = true;
  }
  encoded_condition_.notify_all();
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
  if (writer_worker_.joinable()) {
    writer_worker_.join();
  }
}

bool SensorBagRecorder::PopFrame(int camera_id, SensorBagFrameJob* job) noexcept {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  queue_condition_.wait(lock, [&] {
    return stopping_.load(std::memory_order_acquire) ||
           std::any_of(queue_.begin(), queue_.end(), [camera_id](const SensorBagFrameJob& item) {
             return item.camera_id == camera_id;
           });
  });
  const auto iter = std::find_if(queue_.begin(), queue_.end(),
                                [camera_id](const SensorBagFrameJob& item) {
                                  return item.camera_id == camera_id;
                                });
  if (iter == queue_.end()) {
    return false;
  }
  *job = std::move(*iter);
  queue_.erase(iter);
  return true;
}

bool SensorBagRecorder::PopNextEncodedFrame(EncodedFrameJob* encoded,
                                            uint64_t* order_wait_ns) noexcept {
  if (encoded == nullptr || order_wait_ns == nullptr || encoded_slots_.empty()) {
    return false;
  }
  std::unique_lock<std::mutex> lock(encoded_mutex_);
  bool skipped_evicted_order = false;
  while (true) {
    EncodedFrameSlot& slot = encoded_slots_[next_write_order_ % kEncodedQueueCapacity];
    if (!slot.occupied || slot.record_order != next_write_order_ || !slot.evicted) {
      break;
    }
    slot = EncodedFrameSlot{};
    ++next_write_order_;
    skipped_evicted_order = true;
  }

  EncodedFrameSlot& slot = encoded_slots_[next_write_order_ % kEncodedQueueCapacity];
  if (HasFatalError() || !slot.occupied || slot.evicted ||
      slot.record_order != next_write_order_) {
    lock.unlock();
    if (skipped_evicted_order) {
      encoded_condition_.notify_all();
    }
    return false;
  }

  *order_wait_ns = slot.job.queued_timestamp_ns == 0U
                       ? 0U
                       : SteadyClockNowNs() - slot.job.queued_timestamp_ns;
  *encoded = std::move(slot.job);
  slot = EncodedFrameSlot{};
  --encoded_queue_count_;
  UpdateEncodedQueueAccountingLocked(encoded_queue_count_);
  ++next_write_order_;
  lock.unlock();
  encoded_condition_.notify_all();
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
  return true;
}

void SensorBagRecorder::ReleaseFrameJob(SensorBagFrameJob* job) noexcept {
  if (job == nullptr) {
    return;
  }
  job->raw_frame = QueuedFrame{};
  jpeg_encoder_.ReleaseSlot(job->jpeg_slot);
  job->jpeg_slot = nullptr;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (reusable_jobs_.size() < kReusableJobCapacity) {
    reusable_jobs_.push_back(std::move(*job));
  }
}

SensorBagRecorder::EncodedFrameJob SensorBagRecorder::AcquireEncodedFrameJob() {
  std::lock_guard<std::mutex> lock(payload_pool_mutex_);
  if (reusable_encoded_payloads_.empty()) {
    return EncodedFrameJob{};
  }
  EncodedFrameJob encoded = std::move(reusable_encoded_payloads_.back());
  reusable_encoded_payloads_.pop_back();
  encoded.job = SensorBagFrameJob{};
  encoded.image_payload.clear();
  encoded.metadata_payload.clear();
  encoded.jpeg_size = 0U;
  encoded.queued_timestamp_ns = 0U;
  return encoded;
}

void SensorBagRecorder::ReleaseEncodedFrameJob(EncodedFrameJob* encoded) noexcept {
  if (encoded == nullptr) {
    return;
  }
  encoded->job = SensorBagFrameJob{};
  encoded->image_payload.clear();
  encoded->metadata_payload.clear();
  encoded->jpeg_size = 0U;
  encoded->queued_timestamp_ns = 0U;
  try {
#ifdef RELEASE008_TESTING
    if (fail_next_payload_pool_recycle_for_test_.exchange(
            false, std::memory_order_acq_rel)) {
      throw std::bad_alloc();
    }
#endif
    std::lock_guard<std::mutex> lock(payload_pool_mutex_);
    if (reusable_encoded_payloads_.size() < kReusablePayloadCapacity) {
      reusable_encoded_payloads_.push_back(std::move(*encoded));
    }
  } catch (...) {
    // Recycling is an optimization. Allocation failure releases the local payload instead
    // of escaping this noexcept ownership boundary and terminating the process.
    *encoded = EncodedFrameJob{};
  }
}

size_t SensorBagRecorder::PopImuBatch(icm42688_sample_t* samples,
                                      size_t max_samples) noexcept {
  if (samples == nullptr || max_samples == 0U) {
    return 0U;
  }
  std::lock_guard<std::mutex> lock(imu_queue_mutex_);
  const size_t batch_count = std::min(max_samples, imu_queue_count_);
  if (batch_count == 0U) {
    return 0U;
  }
  const size_t first_count = std::min(batch_count, kImuQueueCapacity - imu_queue_head_);
  std::copy_n(imu_queue_.data() + imu_queue_head_, first_count, samples);
  const size_t second_count = batch_count - first_count;
  if (second_count > 0U) {
    std::copy_n(imu_queue_.data(), second_count, samples + first_count);
  }
  imu_queue_head_ = (imu_queue_head_ + batch_count) % kImuQueueCapacity;
  imu_queue_count_ -= batch_count;
  UpdateImuQueueAccountingLocked(imu_queue_count_);
  return batch_count;
}

bool SensorBagRecorder::WriterInputReady() noexcept {
  if (HasFatalError()) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(encoded_mutex_);
    if (!encoded_slots_.empty()) {
      const EncodedFrameSlot& slot = encoded_slots_[next_write_order_ % kEncodedQueueCapacity];
      if (slot.occupied && slot.record_order == next_write_order_) {
        return true;
      }
      if (encoded_input_closed_) {
        return true;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(imu_queue_mutex_);
    if (imu_queue_count_ > 0U) {
      return true;
    }
  }
  return WriterInputsClosed();
}

bool SensorBagRecorder::WriterInputsClosed() noexcept {
  bool encoded_closed = false;
  bool encoded_empty = false;
  {
    std::lock_guard<std::mutex> lock(encoded_mutex_);
    encoded_closed = encoded_input_closed_;
    encoded_empty = encoded_queue_count_ == 0U;
  }
  bool imu_empty = false;
  {
    std::lock_guard<std::mutex> lock(imu_queue_mutex_);
    imu_empty = imu_queue_count_ == 0U;
  }
  return encoded_closed && encoded_empty && imu_empty;
}

bool SensorBagRecorder::HasClosedEncodedOrderGap() noexcept {
  std::lock_guard<std::mutex> lock(encoded_mutex_);
  return encoded_input_closed_ && encoded_queue_count_ > 0U;
}

SensorBagRecorder::EncodedFrameJob SensorBagRecorder::EncodeFrame(SensorBagFrameJob* job) {
  if (job == nullptr) {
    throw std::runtime_error("missing bag recorder frame job");
  }
  if (job->jpeg_slot == nullptr) {
    X5JpegInputSlot* slot = jpeg_encoder_.AcquireSlot(job->camera_id);
    if (slot == nullptr && !stopping_.load(std::memory_order_acquire)) {
      // staging slot 等待只发生在 recorder 私有 worker，不能阻塞 SC132 callback 或 RTSP worker。
      slot = jpeg_encoder_.WaitAcquireSlot(job->camera_id, stopping_);
    }
    if (slot == nullptr) {
      if (stopping_.load(std::memory_order_acquire)) {
        throw std::runtime_error("bag recorder is stopping");
      }
      SetFatalError("bag recorder hardware JPEG staging exhausted camera=" +
                    std::to_string(job->camera_id));
      throw std::runtime_error(ErrorMessage());
    }
    job->jpeg_slot = slot;
  }

  const uint64_t copy_start_ns = SteadyClockNowNs();
  const X5JpegNv12CopyResult copy_result =
      jpeg_encoder_.CopyNv12ToSlot(job->raw_frame, job->jpeg_slot);
  job->raw_frame.Reset();
  ObserveCopyTiming(SteadyClockNowNs() - copy_start_ns, copy_result);

  const RosbagTime sensor_stamp = RosbagTimeFromNs(job->camera_timestamp_ns);
  const std::string frame_id = CameraFrameId(job->camera_id);

  EncodedFrameJob encoded = AcquireEncodedFrameJob();
  encoded.job.camera_id = job->camera_id;
  encoded.job.sequence = job->sequence;
  encoded.job.frame_id = job->frame_id;
  encoded.job.group_id = job->group_id;
  encoded.job.group_timestamp_ns = job->group_timestamp_ns;
  encoded.job.record_order = job->record_order;
  encoded.job.camera_timestamp_ns = job->camera_timestamp_ns;
  encoded.job.width = job->width;
  encoded.job.height = job->height;
  encoded.job.stride = job->stride;
  encoded.job.vstride = job->vstride;
  const size_t jpeg_size_offset = StartCompressedImagePayload(
      SequenceToRosSeq(job->sequence), sensor_stamp, frame_id, &encoded.image_payload);
  const uint64_t encode_start_ns = SteadyClockNowNs();
  jpeg_encoder_.EncodeAppend(
      X5JpegEncodeRequest{job->camera_id, job->camera_timestamp_ns / 1000U, job->jpeg_slot},
      &encoded.image_payload, &encoded.jpeg_size);
  ObserveJpegTiming(SteadyClockNowNs() - encode_start_ns);
  if (encoded.jpeg_size > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("JPEG payload too large for sensor_msgs/CompressedImage");
  }
  PatchU32(&encoded.image_payload, jpeg_size_offset, static_cast<uint32_t>(encoded.jpeg_size));
  encoded.metadata_payload.clear();
  AppendRosString(&encoded.metadata_payload, FrameMetadataJson(*job, encoded.jpeg_size));
  return encoded;
}

void SensorBagRecorder::PublishEncodedFrame(EncodedFrameJob encoded) {
  const uint64_t record_order = encoded.job.record_order;
  std::unique_lock<std::mutex> lock(encoded_mutex_);
  encoded_condition_.wait(lock, [&] {
    if (HasFatalError() || record_order < next_write_order_) {
      return true;
    }
    const uint64_t window_distance = record_order - next_write_order_;
    if (window_distance >= kEncodedQueueCapacity) {
      return false;
    }
    const EncodedFrameSlot& slot = encoded_slots_[record_order % kEncodedQueueCapacity];
    return !slot.occupied;
  });
  if (HasFatalError()) {
    throw std::runtime_error(ErrorMessage());
  }
  if (record_order < next_write_order_) {
    throw std::runtime_error("late bag recorder record order");
  }

  EncodedFrameSlot& slot = encoded_slots_[record_order % kEncodedQueueCapacity];
  if (slot.occupied) {
    if (slot.record_order == record_order) {
      throw std::runtime_error("duplicate bag recorder record order");
    }
    throw std::runtime_error("bag recorder encoded reorder invariant violated");
  }

  // 编码线程可能乱序完成；窗口外的帧等待 writer 推进，避免未来帧占用缺口帧的 ring slot。
  encoded.queued_timestamp_ns = SteadyClockNowNs();
  slot.record_order = record_order;
  slot.job = std::move(encoded);
  slot.occupied = true;
  slot.evicted = false;
  ++encoded_queue_count_;
  UpdateEncodedQueueAccountingLocked(encoded_queue_count_);
  lock.unlock();
  encoded_condition_.notify_all();
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
}

void SensorBagRecorder::WriteEncodedFrameToBag(const EncodedFrameJob& encoded,
                                               uint64_t order_wait_ns) {
  const uint64_t writer_hold_start_ns = SteadyClockNowNs();
  const RosbagTime record_stamp = RecordDataStamp(encoded.job.camera_timestamp_ns);
  EnsureSessionConfigWritten(record_stamp);
  EnsureCameraInfoWritten(encoded.job.camera_id, record_stamp);
  writer_.WriteMessage(ImageConnection(encoded.job.camera_id), record_stamp,
                       encoded.image_payload);
  writer_.WriteMessage(FrameMetadataConnection(encoded.job.camera_id), record_stamp,
                       encoded.metadata_payload);
  const uint64_t writer_hold_ns = SteadyClockNowNs() - writer_hold_start_ns;
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    ++stats_.image_frames;
    ++stats_.frame_metadata_messages;
    stats_.jpeg_bytes = SaturatingAdd(stats_.jpeg_bytes, encoded.jpeg_size);
    const size_t camera_index = static_cast<size_t>(encoded.job.camera_id);
    if (camera_index < stats_.image_frames_by_camera.size()) {
      ++stats_.image_frames_by_camera[camera_index];
    }
    UpdateWrittenGroupCountLocked();
    if (!stats_.has_image_timestamp) {
      stats_.first_image_timestamp_ns = encoded.job.camera_timestamp_ns;
      stats_.has_image_timestamp = true;
    }
    stats_.last_image_timestamp_ns = encoded.job.camera_timestamp_ns;
  }
  ObserveWriterTiming(order_wait_ns, 0U, writer_hold_ns, true);
}
void SensorBagRecorder::WriteImuSample(const icm42688_sample_t& sample,
                                       std::vector<uint8_t>* payload) {
  if (payload == nullptr) {
    throw std::runtime_error("missing IMU payload buffer");
  }
  payload->clear();
  AppendHeader(payload, SequenceToRosSeq(sample.sample_sequence),
               RosbagTimeFromNs(sample.sample_timestamp_ns), "robobaton_imu_link");
  AppendF64(payload, 0.0);
  AppendF64(payload, 0.0);
  AppendF64(payload, 0.0);
  AppendF64(payload, 1.0);
  for (int i = 0; i < 9; ++i) {
    AppendF64(payload, i == 0 ? -1.0 : 0.0);
  }
  for (double value : sample.gyro_rps) {
    AppendF64(payload, value);
  }
  for (int i = 0; i < 9; ++i) {
    AppendF64(payload, 0.0);
  }
  for (double value : sample.accel_mps2) {
    AppendF64(payload, value);
  }
  for (int i = 0; i < 9; ++i) {
    AppendF64(payload, 0.0);
  }

  const uint64_t writer_hold_start_ns = SteadyClockNowNs();
  const RosbagTime record_stamp = RecordDataStamp(sample.sample_timestamp_ns);
  EnsureSessionConfigWritten(record_stamp);
  writer_.WriteMessage(imu_connection_, record_stamp, *payload);
  const uint64_t writer_hold_ns = SteadyClockNowNs() - writer_hold_start_ns;
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    ++stats_.imu_samples;
    ++stats_.recorder_imu_written;
  }
  ObserveWriterTiming(0U, 0U, writer_hold_ns, false);
}

RosbagTime SensorBagRecorder::RecordDataStamp(uint64_t preferred_time_ns) noexcept {
  if (!has_last_data_record_time_ || preferred_time_ns > last_data_record_time_ns_) {
    last_data_record_time_ns_ = preferred_time_ns;
    has_last_data_record_time_ = true;
  }
  return RosbagTimeFromNs(last_data_record_time_ns_);
}

void SensorBagRecorder::WriteCameraInfoMessage(int camera_id, RosbagTime stamp) {
  std::vector<uint8_t> payload;
  payload.reserve(256U);
  AppendHeader(&payload, 0U, stamp, CameraFrameId(camera_id));
  AppendU32(&payload, static_cast<uint32_t>(OutputHeight(options_)));
  AppendU32(&payload, static_cast<uint32_t>(OutputWidth(options_)));
  AppendRosString(&payload, "");
  AppendU32(&payload, 0U);
  for (int i = 0; i < 9; ++i) {
    AppendF64(&payload, 0.0);
  }
  for (int i = 0; i < 9; ++i) {
    AppendF64(&payload, 0.0);
  }
  for (int i = 0; i < 12; ++i) {
    AppendF64(&payload, 0.0);
  }
  AppendU32(&payload, 0U);
  AppendU32(&payload, 0U);
  AppendU32(&payload, 0U);
  AppendU32(&payload, 0U);
  AppendU32(&payload, 0U);
  AppendU32(&payload, 0U);
  AppendBool(&payload, false);
  writer_.WriteMessage(CameraInfoConnection(camera_id), stamp, payload);
}

void SensorBagRecorder::EnsureSessionConfigWritten(RosbagTime stamp) {
  if (session_config_written_) {
    return;
  }
  writer_.WriteMessage(session_config_connection_, stamp,
                       MakeStringPayload(SessionConfigJson(options_)));
  session_config_written_ = true;
}

void SensorBagRecorder::EnsureCameraInfoWritten(int camera_id, RosbagTime stamp) {
  const size_t index = static_cast<size_t>(camera_id);
  if (index >= camera_info_written_.size() || camera_info_written_[index]) {
    return;
  }
  WriteCameraInfoMessage(camera_id, stamp);
  camera_info_written_[index] = true;
}

void SensorBagRecorder::WriteSessionStatus(SensorBagFinishOutcome outcome, bool data_complete,
                                           const std::string& published_path,
                                           const std::string& session_uuid) {
  const uint64_t status_time_ns = has_last_data_record_time_
                                      ? last_data_record_time_ns_
                                      : (system_clock_ != nullptr
                                             ? system_clock_->MapRawNs(SteadyClockNowNs())
                                             : SteadyClockNowNs());
  const RosbagTime status_stamp = RosbagTimeFromNs(status_time_ns);
  EnsureSessionConfigWritten(status_stamp);
  // 批量统计在终态状态JSON生成前发布，保证前序bag记录已计入writer统计。
  writer_.PublishPendingStats();
  SensorBagRecorderStats stats = SnapshotStats();
  std::ostringstream stream;
  stream << "{\"success\":" << (data_complete ? "true" : "false") << ','
         << "\"outcome\":\"" << SensorBagFinishOutcomeName(outcome) << "\","
         << "\"data_complete\":" << (data_complete ? "true" : "false") << ','
         << "\"published_path\":\"" << JsonEscape(published_path) << "\","
         << "\"session_uuid\":\"" << JsonEscape(session_uuid) << "\","
         << "\"image_frames\":" << stats.image_frames << ','
         << "\"imu_samples\":" << stats.imu_samples << ','
         << "\"frame_queue_peak_depth\":" << stats.frame_queue_peak_depth << ','
         << "\"frame_queue_full_rejects\":" << stats.frame_queue_full_rejects << ','
         << "\"encoded_queue_peak_depth\":" << stats.encoded_queue_peak_depth << ','
         << "\"encoded_queue_full_rejects\":" << stats.encoded_queue_full_rejects << ','
         << "\"encoded_queue_oldest_evicted_groups\":"
         << stats.encoded_queue_oldest_evicted_groups << ','
         << "\"encoded_queue_oldest_evicted_frames\":"
         << stats.encoded_queue_oldest_evicted_frames << ','
         << "\"encoded_queue_oldest_evicted_bytes\":"
         << stats.encoded_queue_oldest_evicted_bytes << ','
         << "\"writer_image_backlog_peak_depth\":" << stats.writer_image_backlog_peak_depth << ','
         << "\"frame_metadata_messages\":" << stats.frame_metadata_messages << ','
         << "\"jpeg_bytes\":" << stats.jpeg_bytes << ','
         << "\"nv12_copy_bytes\":" << stats.nv12_copy_bytes << ','
         << "\"nv12_copy_bulk_plane_count\":" << stats.nv12_copy_bulk_plane_count << ','
         << "\"nv12_copy_bulk_bytes\":" << stats.nv12_copy_bulk_bytes << ','
         << "\"nv12_copy_row_count\":" << stats.nv12_copy_row_count << ','
         << "\"nv12_copy_row_bytes\":" << stats.nv12_copy_row_bytes << ','
         << "\"write_order_wait_max_ns\":" << stats.write_order_wait_max_ns << ','
         << "\"writer_mutex_wait_max_ns\":" << stats.writer_mutex_wait_max_ns << ','
         << "\"writer_mutex_hold_max_ns\":" << stats.writer_mutex_hold_max_ns << ','
         << "\"source_frame_sets_seen\":" << stats.source_frame_sets_seen << ','
         << "\"recorder_selected_groups\":" << stats.recorder_selected_groups << ','
         << "\"recorder_admitted_groups\":" << stats.recorder_admitted_groups << ','
         << "\"recorder_dropped_groups\":" << stats.recorder_dropped_groups << ','
         << "\"recorder_written_groups\":" << stats.recorder_written_groups << ','
         << "\"recorder_imu_admitted\":" << stats.recorder_imu_admitted << ','
         << "\"recorder_imu_written\":" << stats.recorder_imu_written << ','
         << "\"frame_queue_capacity\":" << stats.frame_queue_capacity << ','
         << "\"frame_queue_high_watermark\":" << stats.frame_queue_high_watermark << ','
         << "\"frame_queue_at_capacity_dwell_ns\":"
         << stats.frame_queue_at_capacity_dwell_ns << ','
         << "\"frame_queue_at_capacity_events\":"
         << stats.frame_queue_at_capacity_events << ','
         << "\"encoded_queue_capacity\":" << stats.encoded_queue_capacity << ','
         << "\"encoded_queue_high_watermark\":" << stats.encoded_queue_high_watermark << ','
         << "\"encoded_queue_at_capacity_dwell_ns\":"
         << stats.encoded_queue_at_capacity_dwell_ns << ','
         << "\"encoded_queue_at_capacity_events\":"
         << stats.encoded_queue_at_capacity_events << ','
         << "\"imu_queue_capacity\":" << stats.imu_queue_capacity << ','
         << "\"imu_queue_high_watermark\":" << stats.imu_queue_high_watermark << ','
         << "\"imu_queue_at_capacity_dwell_ns\":"
         << stats.imu_queue_at_capacity_dwell_ns << ','
         << "\"imu_queue_at_capacity_events\":"
         << stats.imu_queue_at_capacity_events;
  AppendLatencyJson(&stream, "nv12_copy", stats.nv12_copy_latency);
  AppendLatencyJson(&stream, "jpeg_encode", stats.jpeg_encode_latency);
  AppendLatencyJson(&stream, "write_order_wait", stats.write_order_wait_latency);
  AppendLatencyJson(&stream, "image_writer_wait", stats.image_writer_wait_latency);
  AppendLatencyJson(&stream, "image_writer_hold", stats.image_writer_hold_latency);
  AppendLatencyJson(&stream, "imu_writer_wait", stats.imu_writer_wait_latency);
  AppendLatencyJson(&stream, "imu_writer_hold", stats.imu_writer_hold_latency);
  AppendRosbagWriterStatsJson(&stream, stats.writer_stats);
  if (HasFatalError()) {
    stream << ",\"error\":\"" << JsonEscape(ErrorMessage()) << "\"";
  }
  stream << '}';

  writer_.WriteMessage(session_status_connection_, status_stamp,
                       MakeStringPayload(stream.str()));
}

void SensorBagRecorder::SetFatalError(const std::string& message) noexcept {
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (!fatal_error_.load(std::memory_order_relaxed)) {
      error_message_ = message;
      fatal_error_.store(true, std::memory_order_release);
      stopping_.store(true, std::memory_order_release);
      g_stop_requested.store(true, std::memory_order_release);
    }
  }
  queue_condition_.notify_all();
  imu_queue_condition_.notify_all();
  encoded_condition_.notify_all();
  jpeg_encoder_.NotifySlotWaiters();
  NotifyWriter(&writer_wakeup_mutex_, &writer_condition_, &writer_wakeup_generation_);
}

std::string SensorBagRecorder::ErrorMessage() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return error_message_;
}

uint32_t SensorBagRecorder::ImageConnection(int camera_id) const {
  return image_connections_.at(static_cast<size_t>(camera_id));
}

uint32_t SensorBagRecorder::CameraInfoConnection(int camera_id) const {
  return camera_info_connections_.at(static_cast<size_t>(camera_id));
}

uint32_t SensorBagRecorder::FrameMetadataConnection(int camera_id) const {
  return frame_metadata_connections_.at(static_cast<size_t>(camera_id));
}

bool SensorBagRecorder::ShouldRecordGroup(uint64_t group_id,
                                          uint64_t /*group_timestamp_ns*/) {
  if (record_target_fps_ == 0U) {
    return false;
  }
  if (options_.record_frame_skip == 0U) {
    return true;
  }

  std::lock_guard<std::mutex> lock(record_selection_mutex_);
  for (const RecordSelection& selection : record_selections_) {
    if (selection.group_id == group_id) {
      return selection.selected;
    }
  }

  // skip-one 按同步组做原子交替；同一 group 的多路回调复用上面的缓存决策。
  bool selected = true;
  if (options_.record_frame_skip == 1U) {
    selected = next_group_should_record_;
    next_group_should_record_ = !next_group_should_record_;
  }

  record_selections_.push_back(RecordSelection{group_id, selected});
  if (record_selections_.size() > kRecordSelectionCacheCapacity) {
    record_selections_.pop_front();
  }
  return selected;
}

}  // namespace robobaton_demo
