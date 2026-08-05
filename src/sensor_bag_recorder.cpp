#include "sensor_bag_recorder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
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
constexpr int kRecorderWorkerNice = 10;

void ApplyRecorderWorkerPriority() noexcept {
#if defined(__linux__)
  const pid_t thread_id = static_cast<pid_t>(::syscall(SYS_gettid));
  if (thread_id > 0) {
    // record-bag 编码和顺序写盘线程降为普通低优先级；失败只保留默认调度，不影响采集主流程。
    static_cast<void>(::setpriority(PRIO_PROCESS, thread_id, kRecorderWorkerNice));
  }
#endif
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

uint64_t Nv12CopiedBytes(const QueuedFrame& frame) noexcept {
  return static_cast<uint64_t>(frame.width) * frame.height * 3U / 2U;
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
  camera_mask_ = options.camera_mask;
  record_target_fps_ = ImagePersistenceFps(options.fps, options.record_frame_skip);
  options_ = options;
  system_clock_ = options.system_clock;
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_ = SensorBagRecorderStats{};
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
  next_write_order_ = 0U;
  record_selections_.clear();
  next_group_should_record_ = true;
  reusable_jobs_.clear();
  reusable_jobs_.reserve(kReusableJobCapacity);
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
    imu_worker_ = std::thread(&SensorBagRecorder::ImuWorkerEntry, this);
  } catch (...) {
    stopping_.store(true, std::memory_order_release);
    queue_condition_.notify_all();
    imu_queue_condition_.notify_all();
    encoded_condition_.notify_all();
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
  if (!ShouldRecordGroup(frame.group_id, frame.group_timestamp_ns)) {
    return;
  }
  if (frame.y_data == nullptr || frame.uv_data == nullptr || frame.width == 0U ||
      frame.height == 0U || frame.stride < frame.width || frame.vstride < frame.height) {
    throw std::runtime_error("invalid frame for bag recorder");
  }
  SensorBagFrameJob job;
  X5JpegInputSlot* slot = jpeg_encoder_.AcquireSlot(frame.channel);
  if (slot == nullptr && !stopping_.load(std::memory_order_acquire)) {
    // 短时JPU/写盘背压只等待自有staging slot；真正无效camera仍按首错失败。
    slot = jpeg_encoder_.WaitAcquireSlot(frame.channel, stopping_);
  }
  if (slot == nullptr) {
    if (stopping_.load(std::memory_order_acquire)) {
      throw std::runtime_error("bag recorder is stopping");
    }
    SetFatalError("bag recorder hardware JPEG staging exhausted camera=" +
                  std::to_string(frame.channel));
    throw std::runtime_error(ErrorMessage());
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      jpeg_encoder_.ReleaseSlot(slot);
      throw std::runtime_error("bag recorder is stopping");
    }
    if (queue_.size() >= kQueueCapacity) {
      {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.frame_queue_full_rejects;
      }
      SetFatalError("bag recorder frame queue full");
      jpeg_encoder_.ReleaseSlot(slot);
      throw std::runtime_error("bag recorder frame queue full");
    }
    if (!reusable_jobs_.empty()) {
      job = std::move(reusable_jobs_.back());
      reusable_jobs_.pop_back();
    }
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
  job.jpeg_slot = slot;
  // 回调只复制被选中的同步组到 recorder 自有 DMA slot；SC132 frame 不被 recorder retain。
  const uint64_t copy_start_ns = SteadyClockNowNs();
  try {
    jpeg_encoder_.CopyNv12ToSlot(frame, slot);
  } catch (...) {
    jpeg_encoder_.ReleaseSlot(slot);
    throw;
  }
  ObserveCopyTiming(SteadyClockNowNs() - copy_start_ns, Nv12CopiedBytes(frame));

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    const bool was_stopping = stopping_.load(std::memory_order_acquire);
    const bool queue_full = queue_.size() >= kQueueCapacity;
    if (was_stopping || queue_full) {
      jpeg_encoder_.ReleaseSlot(job.jpeg_slot);
      job.jpeg_slot = nullptr;
      if (queue_full) {
        {
          std::lock_guard<std::mutex> stats_lock(stats_mutex_);
          ++stats_.frame_queue_full_rejects;
        }
        SetFatalError("bag recorder frame queue full");
      }
      throw std::runtime_error(was_stopping ? "bag recorder is stopping"
                                            : "bag recorder frame queue full");
    }
    job.record_order = next_frame_order_;
    queue_.push_back(std::move(job));
    ++next_frame_order_;
    const uint64_t queue_depth = static_cast<uint64_t>(queue_.size());
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.frame_queue_peak_depth = std::max(stats_.frame_queue_peak_depth, queue_depth);
  }
  queue_condition_.notify_all();
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
      SetFatalError("bag recorder IMU queue full");
      throw std::runtime_error("bag recorder IMU queue full");
    }
    const size_t tail = (imu_queue_head_ + imu_queue_count_) % kImuQueueCapacity;
    imu_queue_[tail] = sample;
    ++imu_queue_count_;
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      stats_.imu_queue_peak_depth = std::max<uint64_t>(
          stats_.imu_queue_peak_depth, static_cast<uint64_t>(imu_queue_count_));
    }
  }
  // IMU callback 只做定长 ring enqueue；ROS 序列化和 bag 写盘由低优先级持久化线程完成，避免阻塞 mapper refresh。
  imu_queue_condition_.notify_one();
}

bool SensorBagRecorder::Finish(bool session_success) noexcept {
  if (!enabled_ || finished_) {
    return !HasFatalError();
  }
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

  bool success = session_success && !HasFatalError();
  std::string cleanup_error;
  const bool jpeg_cleanup_ok = jpeg_encoder_.Stop(&cleanup_error);
  if (!jpeg_cleanup_ok) {
    SetFatalError(cleanup_error.empty() ? "hardware JPEG cleanup failed" : cleanup_error);
    success = false;
  }
  // 失败session只清理当前临时写入，不发布可被后续oracle读取的final bag。
  if (!success) {
    std::lock_guard<std::mutex> writer_lock(writer_mutex_);
    writer_.Abort();
    finished_ = true;
    enabled_ = false;
    return !HasFatalError();
  }
  try {
    std::lock_guard<std::mutex> writer_lock(writer_mutex_);
    WriteSessionStatus(success);
    writer_.Finish();
  } catch (const std::exception& error) {
    SetFatalError(error.what());
    writer_.Abort();
    success = false;
  } catch (...) {
    SetFatalError("unknown recorder finish failure");
    writer_.Abort();
    success = false;
  }
  finished_ = true;
  enabled_ = false;
  return !HasFatalError();
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
  writer_.Abort();
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
  return stats_;
}

void SensorBagRecorder::ObserveCopyTiming(uint64_t copy_ns, uint64_t copy_bytes) noexcept {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_.nv12_copy_bytes = SaturatingAdd(stats_.nv12_copy_bytes, copy_bytes);
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
  ApplyRecorderWorkerPriority();
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
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      break;
    }
  }
}

void SensorBagRecorder::WriterEntry() noexcept {
  ApplyRecorderWorkerPriority();
  while (!HasFatalError()) {
    EncodedFrameJob encoded;
    uint64_t order_wait_ns = 0U;
    if (!PopNextEncodedFrame(&encoded, &order_wait_ns)) {
      break;
    }
    try {
      WriteEncodedFrameToBag(encoded, order_wait_ns);
    } catch (const std::exception& error) {
      SetFatalError(error.what());
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      break;
    } catch (...) {
      SetFatalError("unknown encoded frame write failure");
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      break;
    }
  }
}

void SensorBagRecorder::ImuWorkerEntry() noexcept {
  ApplyRecorderWorkerPriority();
  std::vector<uint8_t> payload;
  payload.reserve(320U);
  while (!HasFatalError()) {
    icm42688_sample_t sample{};
    if (!PopImu(&sample)) {
      break;
    }
    try {
      WriteImuSample(sample, &payload);
    } catch (const std::exception& error) {
      SetFatalError(error.what());
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      break;
    } catch (...) {
      SetFatalError("unknown IMU bag recorder failure");
      stopping_.store(true, std::memory_order_release);
      queue_condition_.notify_all();
      imu_queue_condition_.notify_all();
      encoded_condition_.notify_all();
      break;
    }
  }
}

void SensorBagRecorder::StopWorkers() noexcept {
  if (imu_worker_.joinable()) {
    imu_worker_.join();
  }
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
  const uint64_t order_wait_start_ns = SteadyClockNowNs();
  std::unique_lock<std::mutex> lock(encoded_mutex_);
  const auto has_next_encoded = [&] {
    const EncodedFrameSlot& slot = encoded_slots_[next_write_order_ % kEncodedQueueCapacity];
    return slot.occupied && slot.record_order == next_write_order_;
  };
  // writer 必须等待 next_write_order；输入关闭后仍不跳号，避免生成时间乱序 bag。
  encoded_condition_.wait(lock, [&] {
    return HasFatalError() || encoded_input_closed_ || has_next_encoded();
  });
  *order_wait_ns = SteadyClockNowNs() - order_wait_start_ns;
  if (HasFatalError() || !has_next_encoded()) {
    return false;
  }

  EncodedFrameSlot& slot = encoded_slots_[next_write_order_ % kEncodedQueueCapacity];
  *encoded = std::move(slot.job);
  slot = EncodedFrameSlot{};
  --encoded_queue_count_;
  ++next_write_order_;
  lock.unlock();
  encoded_condition_.notify_all();
  return true;
}

void SensorBagRecorder::ReleaseFrameJob(SensorBagFrameJob* job) noexcept {
  if (job == nullptr) {
    return;
  }
  jpeg_encoder_.ReleaseSlot(job->jpeg_slot);
  job->jpeg_slot = nullptr;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (reusable_jobs_.size() < kReusableJobCapacity) {
    reusable_jobs_.push_back(std::move(*job));
  }
}

bool SensorBagRecorder::PopImu(icm42688_sample_t* sample) noexcept {
  std::unique_lock<std::mutex> lock(imu_queue_mutex_);
  imu_queue_condition_.wait(lock, [&] {
    return stopping_.load(std::memory_order_acquire) || imu_queue_count_ > 0U;
  });
  if (imu_queue_count_ == 0U) {
    return false;
  }
  *sample = imu_queue_[imu_queue_head_];
  imu_queue_head_ = (imu_queue_head_ + 1U) % kImuQueueCapacity;
  --imu_queue_count_;
  return true;
}

SensorBagRecorder::EncodedFrameJob SensorBagRecorder::EncodeFrame(SensorBagFrameJob* job) {
  if (job == nullptr) {
    throw std::runtime_error("missing bag recorder frame job");
  }
  const RosbagTime sensor_stamp = RosbagTimeFromNs(job->camera_timestamp_ns);
  const std::string frame_id = CameraFrameId(job->camera_id);

  EncodedFrameJob encoded;
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
  std::vector<uint8_t>& jpeg = job->jpeg_scratch;
  // JPEG scratch 随可复用 job 回收，避免每帧重新分配硬件输出缓冲。
  if (jpeg.capacity() < kInitialJpegOutputBytes) {
    jpeg.reserve(kInitialJpegOutputBytes);
  }
  const uint64_t encode_start_ns = SteadyClockNowNs();
  jpeg_encoder_.Encode(X5JpegEncodeRequest{job->camera_id, job->camera_timestamp_ns / 1000U,
                                           job->jpeg_slot},
                       &jpeg);
  ObserveJpegTiming(SteadyClockNowNs() - encode_start_ns);
  encoded.jpeg_size = jpeg.size();
  if (encoded.jpeg_size > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("JPEG payload too large for sensor_msgs/CompressedImage");
  }
  encoded.image_payload.insert(encoded.image_payload.end(), jpeg.begin(), jpeg.end());
  PatchU32(&encoded.image_payload, jpeg_size_offset, static_cast<uint32_t>(encoded.jpeg_size));
  encoded.metadata_payload = MakeStringPayload(FrameMetadataJson(*job, encoded.jpeg_size));
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
  slot.record_order = record_order;
  slot.job = std::move(encoded);
  slot.occupied = true;
  ++encoded_queue_count_;
  const uint64_t queue_depth = static_cast<uint64_t>(encoded_queue_count_);
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.encoded_queue_peak_depth = std::max(stats_.encoded_queue_peak_depth, queue_depth);
  }
  lock.unlock();
  encoded_condition_.notify_all();
}

void SensorBagRecorder::WriteEncodedFrameToBag(const EncodedFrameJob& encoded,
                                               uint64_t order_wait_ns) {
  uint64_t writer_wait_ns = 0U;
  uint64_t writer_hold_ns = 0U;
  {
    const uint64_t writer_wait_start_ns = SteadyClockNowNs();
    std::unique_lock<std::mutex> writer_lock(writer_mutex_);
    writer_wait_ns = SteadyClockNowNs() - writer_wait_start_ns;
    const uint64_t writer_hold_start_ns = SteadyClockNowNs();
    const RosbagTime record_stamp = RecordDataStamp(encoded.job.camera_timestamp_ns);
    EnsureSessionConfigWritten(record_stamp);
    EnsureCameraInfoWritten(encoded.job.camera_id, record_stamp);
    writer_.WriteMessage(ImageConnection(encoded.job.camera_id), record_stamp,
                         encoded.image_payload);
    writer_.WriteMessage(FrameMetadataConnection(encoded.job.camera_id), record_stamp,
                         encoded.metadata_payload);
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      ++stats_.image_frames;
      if (encoded.job.camera_id >= 0 && encoded.job.camera_id < kMaxChannels) {
        ++stats_.image_frames_by_camera[static_cast<size_t>(encoded.job.camera_id)];
      }
      if (!stats_.has_image_timestamp) {
        stats_.first_image_timestamp_ns = encoded.job.camera_timestamp_ns;
        stats_.has_image_timestamp = true;
      }
      stats_.last_image_timestamp_ns = encoded.job.camera_timestamp_ns;
      ++stats_.frame_metadata_messages;
      stats_.jpeg_bytes += encoded.jpeg_size;
    }
    writer_hold_ns = SteadyClockNowNs() - writer_hold_start_ns;
  }
  ObserveWriterTiming(order_wait_ns, writer_wait_ns, writer_hold_ns, true);
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

  const uint64_t writer_wait_start_ns = SteadyClockNowNs();
  std::unique_lock<std::mutex> writer_lock(writer_mutex_);
  const uint64_t writer_wait_ns = SteadyClockNowNs() - writer_wait_start_ns;
  const uint64_t writer_hold_start_ns = SteadyClockNowNs();
  const RosbagTime record_stamp = RecordDataStamp(sample.sample_timestamp_ns);
  EnsureSessionConfigWritten(record_stamp);
  writer_.WriteMessage(imu_connection_, record_stamp, *payload);
  writer_lock.unlock();
  const uint64_t writer_hold_ns = SteadyClockNowNs() - writer_hold_start_ns;
  {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    ++stats_.imu_samples;
  }
  ObserveWriterTiming(0U, writer_wait_ns, writer_hold_ns, false);
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

void SensorBagRecorder::WriteSessionStatus(bool success) {
  SensorBagRecorderStats stats = SnapshotStats();
  std::ostringstream stream;
  stream << "{\"success\":" << (success ? "true" : "false") << ','
         << "\"image_frames\":" << stats.image_frames << ','
         << "\"imu_samples\":" << stats.imu_samples << ','
         << "\"frame_queue_peak_depth\":" << stats.frame_queue_peak_depth << ','
         << "\"frame_queue_full_rejects\":" << stats.frame_queue_full_rejects << ','
         << "\"encoded_queue_peak_depth\":" << stats.encoded_queue_peak_depth << ','
         << "\"encoded_queue_full_rejects\":" << stats.encoded_queue_full_rejects << ','
         << "\"frame_metadata_messages\":" << stats.frame_metadata_messages << ','
         << "\"jpeg_bytes\":" << stats.jpeg_bytes << ','
         << "\"nv12_copy_bytes\":" << stats.nv12_copy_bytes << ','
         << "\"write_order_wait_max_ns\":" << stats.write_order_wait_max_ns << ','
         << "\"writer_mutex_wait_max_ns\":" << stats.writer_mutex_wait_max_ns << ','
         << "\"writer_mutex_hold_max_ns\":" << stats.writer_mutex_hold_max_ns;
  AppendLatencyJson(&stream, "nv12_copy", stats.nv12_copy_latency);
  AppendLatencyJson(&stream, "jpeg_encode", stats.jpeg_encode_latency);
  AppendLatencyJson(&stream, "write_order_wait", stats.write_order_wait_latency);
  AppendLatencyJson(&stream, "image_writer_wait", stats.image_writer_wait_latency);
  AppendLatencyJson(&stream, "image_writer_hold", stats.image_writer_hold_latency);
  AppendLatencyJson(&stream, "imu_writer_wait", stats.imu_writer_wait_latency);
  AppendLatencyJson(&stream, "imu_writer_hold", stats.imu_writer_hold_latency);
  if (HasFatalError()) {
    stream << ",\"error\":\"" << JsonEscape(ErrorMessage()) << "\"";
  }
  stream << '}';

  const uint64_t status_time_ns = has_last_data_record_time_
                                      ? last_data_record_time_ns_
                                      : (system_clock_ != nullptr
                                             ? system_clock_->MapRawNs(SteadyClockNowNs())
                                             : SteadyClockNowNs());
  const RosbagTime status_stamp = RosbagTimeFromNs(status_time_ns);
  EnsureSessionConfigWritten(status_stamp);
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
