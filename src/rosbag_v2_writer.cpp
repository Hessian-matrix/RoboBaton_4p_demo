#include "rosbag_v2_writer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <system_error>

#include <unistd.h>

namespace robobaton_demo {
namespace {

constexpr uint8_t kOpMsgData = 0x02U;
constexpr uint8_t kOpFileHeader = 0x03U;
constexpr uint8_t kOpIndexData = 0x04U;
constexpr uint8_t kOpChunk = 0x05U;
constexpr uint8_t kOpChunkInfo = 0x06U;
constexpr uint8_t kOpConnection = 0x07U;
constexpr uint32_t kIndexVersion = 1U;
constexpr uint32_t kChunkInfoVersion = 1U;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr size_t kOutputBufferBytes = 8U * 1024U * 1024U;
constexpr uint64_t kMaxChunkAgeNs = 1ULL * kNanosecondsPerSecond;
constexpr uint64_t kLatencyFirstBucketNs = 1000U;
constexpr uint64_t kStatsPublishRecordInterval = 256U;
constexpr uint64_t kStatsPublishIntervalNs = 100ULL * 1000ULL * 1000ULL;
constexpr size_t kDefaultIndexReservePerChunk = 8U;
constexpr size_t kImageIndexReservePerChunk = 64U;
constexpr size_t kImuIndexReservePerChunk = 1200U;


uint64_t SteadyNowNs() noexcept {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

uint64_t SaturatingAdd(uint64_t lhs, uint64_t rhs) noexcept {
  return rhs > std::numeric_limits<uint64_t>::max() - lhs
             ? std::numeric_limits<uint64_t>::max()
             : lhs + rhs;
}

size_t RosbagLatencyBucketIndex(uint64_t latency_ns) noexcept {
  uint64_t upper_bound_ns = kLatencyFirstBucketNs;
  size_t index = 0U;
  while (latency_ns > upper_bound_ns && index + 1U < kRosbagV2LatencyBucketCount) {
    upper_bound_ns = upper_bound_ns > std::numeric_limits<uint64_t>::max() / 2U
                         ? std::numeric_limits<uint64_t>::max()
                         : upper_bound_ns * 2U;
    ++index;
  }
  return index;
}

uint64_t RosbagLatencyBucketUpperBoundNs(size_t index) noexcept {
  uint64_t upper_bound_ns = kLatencyFirstBucketNs;
  for (size_t i = 0U; i < index; ++i) {
    upper_bound_ns = upper_bound_ns > std::numeric_limits<uint64_t>::max() / 2U
                         ? std::numeric_limits<uint64_t>::max()
                         : upper_bound_ns * 2U;
  }
  return upper_bound_ns;
}

void ObserveRosbagLatency(RosbagV2LatencyStats* stats, uint64_t latency_ns) noexcept {
  if (stats == nullptr) {
    return;
  }
  ++stats->count;
  stats->total_ns = SaturatingAdd(stats->total_ns, latency_ns);
  stats->max_ns = std::max(stats->max_ns, latency_ns);
  ++stats->buckets[RosbagLatencyBucketIndex(latency_ns)];
}

void MergeRosbagLatencyStats(RosbagV2LatencyStats* target,
                             const RosbagV2LatencyStats& delta) noexcept {
  if (target == nullptr || delta.count == 0U) {
    return;
  }
  target->count = SaturatingAdd(target->count, delta.count);
  target->total_ns = SaturatingAdd(target->total_ns, delta.total_ns);
  target->max_ns = std::max(target->max_ns, delta.max_ns);
  for (size_t index = 0U; index < target->buckets.size(); ++index) {
    target->buckets[index] = SaturatingAdd(target->buckets[index], delta.buckets[index]);
  }
}

void MergeRosbagWriterStats(RosbagV2WriterStats* target,
                            const RosbagV2WriterStats& delta) noexcept {
  if (target == nullptr) {
    return;
  }
  MergeRosbagLatencyStats(&target->chunk_open_latency, delta.chunk_open_latency);
  MergeRosbagLatencyStats(&target->chunk_write_latency, delta.chunk_write_latency);
  MergeRosbagLatencyStats(&target->chunk_header_patch_latency,
                          delta.chunk_header_patch_latency);
  MergeRosbagLatencyStats(&target->chunk_index_latency, delta.chunk_index_latency);
  MergeRosbagLatencyStats(&target->chunk_close_latency, delta.chunk_close_latency);
  MergeRosbagLatencyStats(&target->record_write_latency, delta.record_write_latency);
  MergeRosbagLatencyStats(&target->record_header_write_latency,
                          delta.record_header_write_latency);
  MergeRosbagLatencyStats(&target->record_payload_write_latency,
                          delta.record_payload_write_latency);
  MergeRosbagLatencyStats(&target->flush_close_latency, delta.flush_close_latency);
  target->raw_write_calls = SaturatingAdd(target->raw_write_calls, delta.raw_write_calls);
  target->raw_write_bytes = SaturatingAdd(target->raw_write_bytes, delta.raw_write_bytes);
  target->record_header_write_bytes =
      SaturatingAdd(target->record_header_write_bytes, delta.record_header_write_bytes);
  target->record_payload_write_bytes =
      SaturatingAdd(target->record_payload_write_bytes, delta.record_payload_write_bytes);
}

size_t IndexReserveForConnection(const std::string& topic,
                                 const std::string& datatype) {
  if (datatype == "sensor_msgs/Imu" || topic == "/imu/data") {
    return kImuIndexReservePerChunk;
  }
  if (datatype == "sensor_msgs/CompressedImage" ||
      topic.find("/image/") != std::string::npos ||
      topic.find("frame_metadata") != std::string::npos) {
    return kImageIndexReservePerChunk;
  }
  return kDefaultIndexReservePerChunk;
}



void AppendBytes(std::vector<uint8_t>* out, const void* data, size_t size) {
  if (out == nullptr || (data == nullptr && size != 0U)) {
    throw std::invalid_argument("AppendBytes invalid argument");
  }
  if (size == 0U) {
    return;
  }
  if (size > out->max_size() - out->size()) {
    throw std::length_error("ROS buffer too large");
  }
  const size_t offset = out->size();
  out->resize(offset + size);
  std::memcpy(out->data() + offset, data, size);
}

void StartSerializedHeader(std::vector<uint8_t>* out) {
  out->clear();
  AppendU32(out, 0U);
}

void AppendSerializedHeaderField(std::vector<uint8_t>* out, const char* name,
                                 const void* value, size_t value_size) {
  if (name == nullptr || name[0] == '\0') {
    throw std::invalid_argument("empty rosbag header field name");
  }
  const size_t name_size = std::strlen(name);
  const size_t field_size = name_size + 1U + value_size;
  if (field_size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rosbag header field too large");
  }
  AppendU32(out, static_cast<uint32_t>(field_size));
  AppendBytes(out, name, name_size);
  AppendU8(out, '=');
  if (value_size != 0U) {
    AppendBytes(out, value, value_size);
  }
}


void PatchSerializedHeaderLength(std::vector<uint8_t>* out) {
  const size_t header_size = out->size() - sizeof(uint32_t);
  if (header_size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rosbag header too large");
  }
  const uint32_t encoded_header_size = static_cast<uint32_t>(header_size);
  std::memcpy(out->data(), &encoded_header_size, sizeof(encoded_header_size));
}

void FinishSerializedHeader(std::vector<uint8_t>* out, uint32_t data_length) {
  PatchSerializedHeaderLength(out);
  AppendU32(out, data_length);
}

void RequireSafeFinalBagPath(const std::string& final_path) {
  if (final_path.empty() || final_path.front() != '/') {
    throw std::invalid_argument("--record-bag path must be absolute");
  }
  if (final_path.size() < 5U || final_path.substr(final_path.size() - 4U) != ".bag") {
    throw std::invalid_argument("--record-bag path must end with .bag");
  }
  if (final_path.find("/../") != std::string::npos ||
      (final_path.size() >= 3U &&
       final_path.compare(final_path.size() - 3U, 3U, "/..") == 0U)) {
    throw std::invalid_argument("--record-bag path is unsafe");
  }
  for (unsigned char ch : final_path) {
    if (ch < 0x21U || ch > 0x7eU || ch == '\\') {
      throw std::invalid_argument("--record-bag path contains an unsafe character");
    }
  }
}
std::string ErrnoMessage(const std::string& prefix, int error_number) {
  return prefix + ": " + std::strerror(error_number);
}

void AppendCleanupError(std::string* target, const std::string& message) noexcept {
  if (target == nullptr || message.empty()) {
    return;
  }
  try {
    if (!target->empty()) {
      target->append("; ");
    }
    target->append(message);
  } catch (...) {
  }
}

void MarkCleanupError(RosbagV2Writer::AbortResult* result,
                      const std::string& message) noexcept {
  if (result == nullptr) {
    return;
  }
  result->cleanup_complete = false;
  AppendCleanupError(&result->error, message);
}

std::string MakeSessionUuid() {
  static std::atomic<uint64_t> counter{0U};
  timespec realtime{};
  timespec monotonic{};
  (void)::clock_gettime(CLOCK_REALTIME, &realtime);
  (void)::clock_gettime(CLOCK_MONOTONIC, &monotonic);
  std::ostringstream stream;
  stream << std::hex << static_cast<unsigned long long>(realtime.tv_sec) << '-'
         << static_cast<unsigned long long>(realtime.tv_nsec) << '-'
         << static_cast<unsigned long long>(monotonic.tv_sec) << '-'
         << static_cast<unsigned long long>(monotonic.tv_nsec) << '-'
         << static_cast<unsigned long long>(::getpid()) << '-'
         << static_cast<unsigned long long>(counter.fetch_add(1U, std::memory_order_relaxed));
  return stream.str();
}

uint64_t TimeToNs(RosbagTime time) noexcept {
  return static_cast<uint64_t>(time.sec) * kNanosecondsPerSecond + time.nsec;
}

bool ChunkAgeLimitReached(RosbagTime start, RosbagTime end) noexcept {
  const uint64_t start_ns = TimeToNs(start);
  const uint64_t end_ns = TimeToNs(end);
  return end_ns >= start_ns && end_ns - start_ns >= kMaxChunkAgeNs;
}

[[noreturn]] void ThrowErrno(const std::string& prefix) {
  throw std::runtime_error(prefix + ": " + std::strerror(errno));
}

std::vector<std::string> SplitAbsolutePath(const std::string& path) {
  std::vector<std::string> components;
  std::size_t cursor = 1U;
  while (cursor < path.size()) {
    const std::size_t slash = path.find('/', cursor);
    const std::size_t count = slash == std::string::npos ? path.size() - cursor
                                                          : slash - cursor;
    if (count != 0U) {
      components.push_back(path.substr(cursor, count));
    }
    if (slash == std::string::npos) {
      break;
    }
    cursor = slash + 1U;
  }
  return components;
}

int OpenManagedParentDirectory(const std::filesystem::path& parent) {
  const std::string parent_text = parent.string();
  int current_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current_fd < 0) {
    ThrowErrno("open parent root failed");
  }
  for (const std::string& component : SplitAbsolutePath(parent_text)) {
    if (::mkdirat(current_fd, component.c_str(), 0755) != 0 && errno != EEXIST) {
      const int saved_errno = errno;
      ::close(current_fd);
      errno = saved_errno;
      ThrowErrno("create bag parent directory failed");
    }
    const int next_fd =
        ::openat(current_fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next_fd < 0) {
      const int saved_errno = errno;
      ::close(current_fd);
      if (saved_errno == ELOOP) {
        throw std::invalid_argument("--record-bag parent directory must not be a symlink");
      }
      errno = saved_errno;
      ThrowErrno("open bag parent directory failed");
    }
    ::close(current_fd);
    current_fd = next_fd;
  }
  return current_fd;
}

void RequireSafeLeafPath(int parent_dir_fd, const std::string& name, const char* label) {
  struct stat status {};
  if (::fstatat(parent_dir_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return;
    }
    ThrowErrno(std::string("stat ") + label + " failed");
  }
  if (S_ISLNK(status.st_mode)) {
    throw std::invalid_argument(std::string("--record-bag ") + label + " must not be a symlink");
  }
  if (!S_ISREG(status.st_mode)) {
    throw std::invalid_argument(std::string("--record-bag ") + label +
                                " must be a regular file when it already exists");
  }
}

void RequireSiblingAbsent(int parent_dir_fd, const std::string& name, const char* label) {
  struct stat status {};
  if (::fstatat(parent_dir_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return;
    }
    ThrowErrno(std::string("stat ") + label + " failed");
  }
  throw std::runtime_error(std::string("refusing to reuse stale or active ") + label);
}

std::string FsyncFdError(int fd, const char* label) {
  for (;;) {
    if (::fsync(fd) == 0) {
      return std::string();
    }
    if (errno != EINTR) {
      return ErrnoMessage(std::string("fsync ") + label + " failed", errno);
    }
  }
}

void FsyncFd(int fd, const char* label) {
  const std::string error = FsyncFdError(fd, label);
  if (!error.empty()) {
    throw std::runtime_error(error);
  }
}


}  // namespace

RosbagV2Writer::~RosbagV2Writer() { Abort(); }
uint64_t RosbagV2LatencyAverageNs(const RosbagV2LatencyStats& stats) noexcept {
  return stats.count == 0U ? 0U : stats.total_ns / stats.count;
}

uint64_t RosbagV2LatencyPercentileUpperNs(const RosbagV2LatencyStats& stats,
                                          uint32_t percentile) noexcept {
  if (stats.count == 0U) {
    return 0U;
  }
  const uint64_t rank = (stats.count * std::min<uint32_t>(percentile, 100U) + 99U) / 100U;
  uint64_t cumulative = 0U;
  for (size_t index = 0U; index < stats.buckets.size(); ++index) {
    cumulative = SaturatingAdd(cumulative, stats.buckets[index]);
    if (cumulative >= rank) {
      return RosbagLatencyBucketUpperBoundNs(index);
    }
  }
  return stats.max_ns;
}

RosbagV2WriterStats RosbagV2Writer::SnapshotStats() const noexcept {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

void RosbagV2Writer::ObserveWriterLatency(
    RosbagV2LatencyStats RosbagV2WriterStats::* target,
    uint64_t latency_ns) noexcept {
  ObserveRosbagLatency(&(pending_stats_.*target), latency_ns);
  ++pending_stats_updates_;
}

void RosbagV2Writer::RecordRawWriteStats(size_t size) noexcept {
  ++pending_stats_.raw_write_calls;
  pending_stats_.raw_write_bytes = SaturatingAdd(pending_stats_.raw_write_bytes, size);
  ++pending_stats_updates_;
}

void RosbagV2Writer::RecordRecordHeaderWriteStats(uint64_t latency_ns,
                                                  size_t bytes) noexcept {
  ObserveRosbagLatency(&pending_stats_.record_header_write_latency, latency_ns);
  pending_stats_.record_header_write_bytes =
      SaturatingAdd(pending_stats_.record_header_write_bytes, bytes);
  ++pending_stats_updates_;
}

void RosbagV2Writer::RecordRecordPayloadWriteStats(uint64_t latency_ns,
                                                   size_t bytes) noexcept {
  ObserveRosbagLatency(&pending_stats_.record_payload_write_latency, latency_ns);
  pending_stats_.record_payload_write_bytes =
      SaturatingAdd(pending_stats_.record_payload_write_bytes, bytes);
  ++pending_stats_updates_;
}

void RosbagV2Writer::PublishPendingStats() noexcept { PublishPendingStats(SteadyNowNs()); }

void RosbagV2Writer::PublishPendingStats(uint64_t publish_ns) noexcept {
  if (pending_stats_updates_ != 0U) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    MergeRosbagWriterStats(&stats_, pending_stats_);
    pending_stats_ = RosbagV2WriterStats{};
    pending_stats_updates_ = 0U;
    pending_stats_record_count_ = 0U;
  }
  last_stats_publish_ns_ = publish_ns;
}

void RosbagV2Writer::MaybePublishPendingStats(uint64_t now_ns) noexcept {
  if (pending_stats_updates_ == 0U) {
    return;
  }
  if (pending_stats_record_count_ >= kStatsPublishRecordInterval ||
      (last_stats_publish_ns_ != 0U &&
       now_ns - last_stats_publish_ns_ >= kStatsPublishIntervalNs)) {
    PublishPendingStats(now_ns);
  }
}


void RosbagV2Writer::Open(const std::string& final_path) {
  RequireSafeFinalBagPath(final_path);
  if (IsOpen()) {
    throw std::logic_error("rosbag writer already open");
  }

  final_path_ = final_path;
  file_header_pos_ = 0U;
  logical_offset_ = 0U;
  index_data_pos_ = 0U;
  chunk_open_ = false;
  current_chunk_pos_ = 0U;
  current_chunk_data_pos_ = 0U;
  current_chunk_ = ChunkInfo{};
  chunks_.clear();
  current_chunk_indexes_.clear();
  connections_.clear();
  connection_index_reserves_.clear();
  header_scratch_.clear();
  if (write_buffer_.size() != kOutputBufferBytes) {
    write_buffer_.resize(kOutputBufferBytes);
  }
  write_buffer_used_ = 0U;
  has_last_message_time_ = false;
  last_message_time_ = RosbagTime{};
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = RosbagV2WriterStats{};
  }
  pending_stats_ = RosbagV2WriterStats{};
  pending_stats_updates_ = 0U;
  pending_stats_record_count_ = 0U;
  last_stats_publish_ns_ = SteadyNowNs();
  partial_path_.clear();
  session_uuid_ = MakeSessionUuid();

  const std::filesystem::path final_file(final_path_);
  const std::filesystem::path parent = final_file.parent_path();
  if (parent.empty()) {
    throw std::invalid_argument("bag path has no parent directory");
  }
  final_name_ = final_file.filename().string();
  partial_path_ = (parent / PartialName()).string();
  temp_name_ = final_name_ + ".tmp";
  lock_name_ = final_name_ + ".lock";
  temp_path_ = (parent / temp_name_).string();
  lock_path_ = (parent / lock_name_).string();
  parent_dir_fd_ = OpenManagedParentDirectory(parent);
  try {
    RequireSafeLeafPath(parent_dir_fd_, final_name_, "path");
    RequireSiblingAbsent(parent_dir_fd_, temp_name_, "temporary bag");
    RequireSiblingAbsent(parent_dir_fd_, lock_name_, "bag lock");
    lock_fd_ = ::openat(parent_dir_fd_, lock_name_.c_str(),
                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd_ < 0) {
      ThrowErrno("create bag lock failed");
    }
    temp_fd_ = ::openat(parent_dir_fd_, temp_name_.c_str(),
                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (temp_fd_ < 0) {
      ThrowErrno("create temporary bag failed");
    }
  } catch (...) {
    ReleaseStagingFiles();
    throw;
  }


  const char version[] = "#ROSBAG V2.0\n";
  WriteRaw(version, sizeof(version) - 1U);
  file_header_pos_ = Tell();
  WriteFileHeaderRecord();
  // 初始文件头占位先落到顺序流，Finish回填文件头时不会被缓冲数据覆盖。
  FlushOutputBuffer();
}

uint32_t RosbagV2Writer::AddConnection(const std::string& topic,
                                       const std::string& datatype,
                                       const std::string& md5sum,
                                       const std::string& message_definition) {
  EnsureOpen();
  if (chunk_open_) {
    throw std::logic_error("connections must be added before messages");
  }
  if (topic.empty() || topic.front() != '/' || datatype.empty() || md5sum.size() != 32U) {
    throw std::invalid_argument("invalid rosbag connection metadata");
  }
  for (const RosbagConnection& connection : connections_) {
    if (connection.topic == topic) {
      throw std::invalid_argument("duplicate rosbag topic: " + topic);
    }
  }

  const size_t index_reserve = IndexReserveForConnection(topic, datatype);
  connections_.reserve(connections_.size() + 1U);
  connection_index_reserves_.reserve(connection_index_reserves_.size() + 1U);
  RosbagConnection connection;
  connection.id = static_cast<uint32_t>(connections_.size());
  connection.topic = topic;
  connection.datatype = datatype;
  connection.md5sum = md5sum;
  connection.message_definition = message_definition;
  connections_.push_back(std::move(connection));
  connection_index_reserves_.push_back(index_reserve);
  return connections_.back().id;
}

void RosbagV2Writer::WriteMessage(uint32_t connection_id, RosbagTime time,
                                  const std::vector<uint8_t>& payload) {
  try {
    const uint64_t record_start_ns = SteadyNowNs();
    EnsureOpen();
    if (connection_id >= connections_.size()) {
      throw std::invalid_argument("unknown rosbag connection id");
    }
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("rosbag message payload too large");
    }
    const RosbagTime record_time = NormalizeMessageTime(time);
    if (!chunk_open_) {
      StartChunk(record_time);
    }

    IndexEntry index;
    index.time = record_time;
    index.offset = CurrentChunkOffset();
    CurrentChunkIndex(connection_id).push_back(index);
    ++current_chunk_.connection_counts[connection_id];
    const uint64_t chunk_write_start_ns = SteadyNowNs();

    WriteMessageRecord(connection_id, record_time, payload);
    ObserveWriterLatency(&RosbagV2WriterStats::chunk_write_latency,
                         SteadyNowNs() - chunk_write_start_ns);

    if (record_time.sec > current_chunk_.end_time.sec ||
        (record_time.sec == current_chunk_.end_time.sec &&
         record_time.nsec > current_chunk_.end_time.nsec)) {
      current_chunk_.end_time = record_time;
    } else if (record_time.sec < current_chunk_.start_time.sec ||
               (record_time.sec == current_chunk_.start_time.sec &&
                record_time.nsec < current_chunk_.start_time.nsec)) {
      current_chunk_.start_time = record_time;
    }

    const uint32_t chunk_offset_after_record = CurrentChunkOffset();
    if (chunk_offset_after_record > kChunkThresholdBytes ||
        ChunkAgeLimitReached(current_chunk_.start_time, current_chunk_.end_time)) {
      StopChunk();
    }
    const uint64_t record_finish_ns = SteadyNowNs();
    ObserveWriterLatency(&RosbagV2WriterStats::record_write_latency,
                         record_finish_ns - record_start_ns);
    ++pending_stats_record_count_;
    MaybePublishPendingStats(record_finish_ns);
  } catch (...) {
    PublishPendingStats();
    throw;
  }
}

RosbagV2Writer::PublishResult RosbagV2Writer::Finish(bool data_complete) {
  try {
    EnsureOpen();
    PublishResult result;
    result.data_complete = data_complete;
    result.session_uuid = session_uuid_;
    result.outcome = data_complete ? PublishOutcome::kPublishedComplete
                                   : PublishOutcome::kPublishedPartial;
    const std::string target_name = data_complete ? final_name_ : PartialName();
    const std::string target_path = data_complete ? final_path_ : partial_path_;
    result.published_path = target_path;

    if (chunk_open_) {
      StopChunk();
    }
    index_data_pos_ = Tell();
    WriteConnectionRecords();
    WriteChunkInfoRecords();
    PatchFileHeaderRecord();
    FlushOutputBuffer();
    const uint64_t flush_close_start_ns = SteadyNowNs();
    FsyncFd(temp_fd_, "temporary bag");
    if (::close(temp_fd_) != 0) {
      const int saved_errno = errno;
      temp_fd_ = -1;
      throw std::runtime_error(ErrnoMessage("close temporary bag failed", saved_errno));
    }
    temp_fd_ = -1;
    ObserveWriterLatency(&RosbagV2WriterStats::flush_close_latency,
                         SteadyNowNs() - flush_close_start_ns);
    RequireSafeLeafPath(parent_dir_fd_, target_name, data_complete ? "path" : "partial bag");
    if (::renameat(parent_dir_fd_, temp_name_.c_str(), parent_dir_fd_, target_name.c_str()) != 0) {
      ThrowErrno(data_complete ? "publish final bag failed" : "publish partial bag failed");
    }

    const std::string parent_sync_error = FsyncFdError(parent_dir_fd_, "bag parent directory");
    if (!parent_sync_error.empty()) {
      result.outcome = PublishOutcome::kPublishedDurabilityUnproven;
      result.error = parent_sync_error;
      const std::string quarantine_name = QuarantineName(target_name);
      const std::filesystem::path quarantine_path =
          std::filesystem::path(target_path).parent_path() / quarantine_name;
      try {
        RequireSiblingAbsent(parent_dir_fd_, quarantine_name, "quarantine bag");
        if (::renameat(parent_dir_fd_, target_name.c_str(), parent_dir_fd_,
                       quarantine_name.c_str()) == 0) {
          result.quarantine_path = quarantine_path.string();
          result.published_path = result.quarantine_path;
          const std::string quarantine_sync_error =
              FsyncFdError(parent_dir_fd_, "quarantine parent directory");
          AppendCleanupError(&result.error, quarantine_sync_error);
        } else {
          AppendCleanupError(&result.error,
                             ErrnoMessage("quarantine unproven bag failed", errno));
        }
      } catch (const std::exception& error) {
        AppendCleanupError(&result.error, error.what());
      } catch (...) {
        AppendCleanupError(&result.error, "unknown quarantine failure");
      }
    }

    const AbortResult cleanup = ReleaseStagingFiles();
    result.cleanup_complete = cleanup.cleanup_complete;
    AppendCleanupError(&result.error, cleanup.error);
    PublishPendingStats();
    return result;
  } catch (...) {
    PublishPendingStats();
    throw;
  }
}

RosbagV2Writer::AbortResult RosbagV2Writer::Abort() noexcept {
  AbortResult result;
  const AbortResult staging = ReleaseStagingFiles();
  if (!staging.cleanup_complete) {
    result.cleanup_complete = false;
    AppendCleanupError(&result.error, staging.error);
  }
  PublishPendingStats();
  return result;
}

void RosbagV2Writer::EnsureOpen() const {
  if (temp_fd_ < 0) {
    throw std::logic_error("rosbag writer is not open");
  }
}

uint64_t RosbagV2Writer::Tell() const noexcept { return logical_offset_; }

void RosbagV2Writer::WriteRaw(const void* data, size_t size) {
  const BufferView buffer{data, size};
  WriteRawBuffers(&buffer, 1U);
}

void RosbagV2Writer::FlushOutputBuffer() {
  EnsureOpen();
  const auto* cursor = write_buffer_.data();
  size_t remaining = write_buffer_used_;
  while (remaining != 0U) {
    const ssize_t written = ::write(temp_fd_, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("write bag failed");
    }
    if (written == 0) {
      throw std::runtime_error("write bag made no progress");
    }
    const size_t bytes_written = static_cast<size_t>(written);
    RecordRawWriteStats(bytes_written);
    cursor += bytes_written;
    remaining -= bytes_written;
  }
  write_buffer_used_ = 0U;
}

void RosbagV2Writer::WriteRawBuffers(const BufferView* buffers, size_t count) {
  EnsureOpen();
  if (buffers == nullptr && count != 0U) {
    throw std::invalid_argument("null bag write buffers");
  }
  if (write_buffer_.empty()) {
    write_buffer_.resize(kOutputBufferBytes);
    write_buffer_used_ = 0U;
  }

  for (size_t index = 0U; index < count; ++index) {
    if (buffers[index].data == nullptr && buffers[index].size != 0U) {
      throw std::invalid_argument("null bag write buffer");
    }
    const auto* cursor = static_cast<const uint8_t*>(buffers[index].data);
    size_t remaining = buffers[index].size;
    while (remaining != 0U) {
      if (write_buffer_used_ == write_buffer_.size()) {
        FlushOutputBuffer();
      }
      const size_t available = write_buffer_.size() - write_buffer_used_;
      const size_t copy_size = std::min(remaining, available);
      std::memcpy(write_buffer_.data() + write_buffer_used_, cursor, copy_size);
      write_buffer_used_ += copy_size;
      logical_offset_ += copy_size;
      cursor += copy_size;
      remaining -= copy_size;
    }
  }
}

void RosbagV2Writer::PwriteRaw(uint64_t offset, const void* data, size_t size) {
  EnsureOpen();
  if (data == nullptr && size != 0U) {
    throw std::invalid_argument("null bag pwrite buffer");
  }
  // pwrite不移动顺序写偏移；调用方保证目标头部已经不在write_buffer_中。

  const auto* cursor = static_cast<const uint8_t*>(data);
  size_t remaining = size;
  uint64_t cursor_offset = offset;
  while (remaining != 0U) {
    if (cursor_offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      throw std::out_of_range("bag pwrite offset too large");
    }
    const ssize_t written = ::pwrite(temp_fd_, cursor, remaining,
                                    static_cast<off_t>(cursor_offset));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("pwrite bag failed");
    }
    if (written == 0) {
      throw std::runtime_error("pwrite bag made no progress");
    }
    const size_t bytes_written = static_cast<size_t>(written);
    RecordRawWriteStats(bytes_written);
    cursor += bytes_written;
    cursor_offset += bytes_written;
    remaining -= bytes_written;
  }
}

void RosbagV2Writer::WriteU32(uint32_t value) { WriteRaw(&value, sizeof(value)); }

size_t RosbagV2Writer::WriteHeader(const std::vector<HeaderField>& fields) {
  StartSerializedHeader(&header_scratch_);
  for (const HeaderField& field : fields) {
    AppendSerializedHeaderField(&header_scratch_, field.name.c_str(),
                                field.value.data(), field.value.size());
  }
  PatchSerializedHeaderLength(&header_scratch_);
  WriteRaw(header_scratch_.data(), header_scratch_.size());
  return header_scratch_.size();
}

size_t RosbagV2Writer::WriteMessageRecord(uint32_t connection_id, RosbagTime time,
                                          const std::vector<uint8_t>& payload) {
  const uint64_t header_start_ns = SteadyNowNs();
  const uint8_t op = kOpMsgData;
  StartSerializedHeader(&header_scratch_);
  AppendSerializedHeaderField(&header_scratch_, "op", &op, sizeof(op));
  AppendSerializedHeaderField(&header_scratch_, "conn", &connection_id, sizeof(connection_id));
  AppendSerializedHeaderField(&header_scratch_, "time", &time, sizeof(time));
  FinishSerializedHeader(&header_scratch_, static_cast<uint32_t>(payload.size()));

  const BufferView buffers[] = {
      {header_scratch_.data(), header_scratch_.size()},
      {payload.empty() ? nullptr : payload.data(), payload.size()},
  };
  const uint64_t write_start_ns = SteadyNowNs();
  WriteRawBuffers(buffers, sizeof(buffers) / sizeof(buffers[0]));
  const uint64_t write_finish_ns = SteadyNowNs();
  RecordRecordHeaderWriteStats(write_finish_ns - header_start_ns,
                               header_scratch_.size());
  RecordRecordPayloadWriteStats(write_finish_ns - write_start_ns, payload.size());
  return header_scratch_.size();
}

void RosbagV2Writer::WriteDataLength(uint32_t value) { WriteU32(value); }

std::vector<RosbagV2Writer::IndexEntry>& RosbagV2Writer::CurrentChunkIndex(
    uint32_t connection_id) {
  if (connection_id >= current_chunk_indexes_.size()) {
    throw std::invalid_argument("unknown rosbag connection id");
  }
  return current_chunk_indexes_[connection_id];
}

void RosbagV2Writer::SerializeFileHeaderRecord() {
  const uint32_t connection_count = static_cast<uint32_t>(connections_.size());
  const uint32_t chunk_count = static_cast<uint32_t>(chunks_.size());
  const uint8_t op = kOpFileHeader;

  StartSerializedHeader(&header_scratch_);
  AppendSerializedHeaderField(&header_scratch_, "op", &op, sizeof(op));
  AppendSerializedHeaderField(&header_scratch_, "index_pos", &index_data_pos_,
                              sizeof(index_data_pos_));
  AppendSerializedHeaderField(&header_scratch_, "conn_count", &connection_count,
                              sizeof(connection_count));
  AppendSerializedHeaderField(&header_scratch_, "chunk_count", &chunk_count,
                              sizeof(chunk_count));
  PatchSerializedHeaderLength(&header_scratch_);
  const size_t header_size = header_scratch_.size() - sizeof(uint32_t);
  if (header_size > kFileHeaderLength) {
    throw std::runtime_error("rosbag file header exceeds reserved length");
  }
  const uint32_t padding_size = kFileHeaderLength - static_cast<uint32_t>(header_size);
  AppendU32(&header_scratch_, padding_size);
  header_scratch_.insert(header_scratch_.end(), padding_size, static_cast<uint8_t>(' '));
}

void RosbagV2Writer::WriteFileHeaderRecord() {
  SerializeFileHeaderRecord();
  WriteRaw(header_scratch_.data(), header_scratch_.size());
}

void RosbagV2Writer::PatchFileHeaderRecord() {
  SerializeFileHeaderRecord();
  PwriteRaw(file_header_pos_, header_scratch_.data(), header_scratch_.size());
}

void RosbagV2Writer::StartChunk(RosbagTime time) {
  const uint64_t start_ns = SteadyNowNs();
  current_chunk_ = ChunkInfo{};
  current_chunk_.pos = Tell();
  current_chunk_.start_time = time;
  current_chunk_.end_time = time;
  current_chunk_.connection_counts.assign(connections_.size(), 0U);
  current_chunk_pos_ = current_chunk_.pos;
  current_chunk_indexes_.clear();
  current_chunk_indexes_.resize(connections_.size());
  for (size_t connection_id = 0U; connection_id < current_chunk_indexes_.size(); ++connection_id) {
    const size_t reserve = connection_id < connection_index_reserves_.size()
                               ? connection_index_reserves_[connection_id]
                               : kDefaultIndexReservePerChunk;
    current_chunk_indexes_[connection_id].reserve(reserve);
  }
  // chunk头占位先落到顺序流，StopChunk回填长度时不会触碰仍在缓冲区中的payload。
  WriteChunkHeader(0U, 0U);
  FlushOutputBuffer();
  current_chunk_data_pos_ = Tell();
  chunk_open_ = true;
  ObserveWriterLatency(&RosbagV2WriterStats::chunk_open_latency,
                       SteadyNowNs() - start_ns);
}

void RosbagV2Writer::StopChunk() {
  if (!chunk_open_) {
    return;
  }
  const uint64_t close_start_ns = SteadyNowNs();
  const uint64_t end_of_chunk_pos = Tell();
  const uint64_t chunk_size = end_of_chunk_pos - current_chunk_data_pos_;
  if (chunk_size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rosbag chunk too large");
  }

  const uint64_t patch_start_ns = SteadyNowNs();
  PatchChunkHeader(static_cast<uint32_t>(chunk_size), static_cast<uint32_t>(chunk_size));
  ObserveWriterLatency(&RosbagV2WriterStats::chunk_header_patch_latency,
                       SteadyNowNs() - patch_start_ns);
  const uint64_t index_start_ns = SteadyNowNs();
  WriteIndexRecords();
  ObserveWriterLatency(&RosbagV2WriterStats::chunk_index_latency,
                       SteadyNowNs() - index_start_ns);
  chunks_.push_back(current_chunk_);
  current_chunk_indexes_.clear();
  current_chunk_ = ChunkInfo{};
  chunk_open_ = false;
  ObserveWriterLatency(&RosbagV2WriterStats::chunk_close_latency,
                       SteadyNowNs() - close_start_ns);
}

void RosbagV2Writer::SerializeChunkHeader(uint32_t compressed_size,
                                          uint32_t uncompressed_size) {
  const uint8_t op = kOpChunk;
  constexpr char kCompression[] = "none";
  StartSerializedHeader(&header_scratch_);
  AppendSerializedHeaderField(&header_scratch_, "op", &op, sizeof(op));
  AppendSerializedHeaderField(&header_scratch_, "compression", kCompression,
                              sizeof(kCompression) - 1U);
  AppendSerializedHeaderField(&header_scratch_, "size", &uncompressed_size,
                              sizeof(uncompressed_size));
  FinishSerializedHeader(&header_scratch_, compressed_size);
}

void RosbagV2Writer::WriteChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size) {
  SerializeChunkHeader(compressed_size, uncompressed_size);
  WriteRaw(header_scratch_.data(), header_scratch_.size());
}

void RosbagV2Writer::PatchChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size) {
  SerializeChunkHeader(compressed_size, uncompressed_size);
  PwriteRaw(current_chunk_pos_, header_scratch_.data(), header_scratch_.size());
}

void RosbagV2Writer::WriteIndexRecords() {
  for (size_t connection_index = 0U; connection_index < current_chunk_indexes_.size();
       ++connection_index) {
    if (connection_index > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("rosbag connection id too large");
    }
    const uint32_t connection_id = static_cast<uint32_t>(connection_index);
    const std::vector<IndexEntry>& index = current_chunk_indexes_[connection_index];
    if (index.empty()) {
      continue;
    }
    if (index.size() > std::numeric_limits<uint32_t>::max() / 12U) {
      throw std::length_error("rosbag index payload too large");
    }
    const uint32_t count = static_cast<uint32_t>(index.size());
    const uint8_t op = kOpIndexData;
    StartSerializedHeader(&header_scratch_);
    AppendSerializedHeaderField(&header_scratch_, "op", &op, sizeof(op));
    AppendSerializedHeaderField(&header_scratch_, "conn", &connection_id,
                                sizeof(connection_id));
    AppendSerializedHeaderField(&header_scratch_, "ver", &kIndexVersion,
                                sizeof(kIndexVersion));
    AppendSerializedHeaderField(&header_scratch_, "count", &count, sizeof(count));
    FinishSerializedHeader(&header_scratch_, count * 12U);

    index_scratch_.clear();
    index_scratch_.reserve(static_cast<size_t>(count) * 12U);
    for (const IndexEntry& entry : index) {
      AppendU32(&index_scratch_, entry.time.sec);
      AppendU32(&index_scratch_, entry.time.nsec);
      AppendU32(&index_scratch_, entry.offset);
    }
    const BufferView buffers[] = {
        {header_scratch_.data(), header_scratch_.size()},
        {index_scratch_.data(), index_scratch_.size()},
    };
    WriteRawBuffers(buffers, sizeof(buffers) / sizeof(buffers[0]));
  }
}

void RosbagV2Writer::WriteConnectionRecords() {
  for (const RosbagConnection& connection : connections_) {
    WriteConnectionRecord(connection);
  }
}

void RosbagV2Writer::WriteConnectionRecord(const RosbagConnection& connection) {
  WriteHeader({FieldU8("op", kOpConnection), FieldString("topic", connection.topic),
               FieldU32("conn", connection.id)});
  WriteHeader({FieldString("type", connection.datatype),
               FieldString("md5sum", connection.md5sum),
               FieldString("message_definition", connection.message_definition)});
}

void RosbagV2Writer::WriteChunkInfoRecords() {
  for (const ChunkInfo& chunk : chunks_) {
    size_t nonzero_count = 0U;
    for (const uint32_t connection_count : chunk.connection_counts) {
      if (connection_count != 0U) {
        ++nonzero_count;
      }
    }
    if (nonzero_count > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("rosbag chunk info too large");
    }
    const uint32_t count = static_cast<uint32_t>(nonzero_count);
    WriteHeader({FieldU8("op", kOpChunkInfo), FieldU32("ver", kChunkInfoVersion),
                 FieldU64("chunk_pos", chunk.pos), FieldTime("start_time", chunk.start_time),
                 FieldTime("end_time", chunk.end_time), FieldU32("count", count)});
    WriteDataLength(count * 8U);
    for (size_t connection_index = 0U; connection_index < chunk.connection_counts.size();
         ++connection_index) {
      const uint32_t connection_count = chunk.connection_counts[connection_index];
      if (connection_count == 0U) {
        continue;
      }
      if (connection_index > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("rosbag connection id too large");
      }
      WriteU32(static_cast<uint32_t>(connection_index));
      WriteU32(connection_count);
    }
  }
}

RosbagTime RosbagV2Writer::NormalizeMessageTime(RosbagTime time) noexcept {
  if (!has_last_message_time_) {
    has_last_message_time_ = true;
    last_message_time_ = time;
    return time;
  }
  if (time.sec < last_message_time_.sec ||
      (time.sec == last_message_time_.sec && time.nsec < last_message_time_.nsec)) {
    return last_message_time_;
  }
  last_message_time_ = time;
  return time;
}

uint32_t RosbagV2Writer::CurrentChunkOffset() {
  const uint64_t current_pos = Tell();
  if (current_pos < current_chunk_data_pos_) {
    throw std::runtime_error("bag write cursor moved before chunk start");
  }
  const uint64_t offset = current_pos - current_chunk_data_pos_;
  if (offset > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rosbag chunk offset too large");
  }
  return static_cast<uint32_t>(offset);
}

std::string RosbagV2Writer::PartialName() const {
  constexpr char kBagSuffix[] = ".bag";
  if (final_name_.size() > 4U &&
      final_name_.compare(final_name_.size() - 4U, 4U, kBagSuffix) == 0) {
    return final_name_.substr(0U, final_name_.size() - 4U) + ".partial.bag";
  }
  return final_name_ + ".partial.bag";
}

std::string RosbagV2Writer::PartialPath() const {
  const std::filesystem::path final_file(final_path_);
  return (final_file.parent_path() / PartialName()).string();
}

std::string RosbagV2Writer::QuarantineName(const std::string& target_name) const {
  constexpr char kBagSuffix[] = ".bag";
  const std::string suffix = ".quarantine-" + session_uuid_ + ".bag";
  if (target_name.size() > 4U &&
      target_name.compare(target_name.size() - 4U, 4U, kBagSuffix) == 0) {
    return target_name.substr(0U, target_name.size() - 4U) + suffix;
  }
  return target_name + suffix;
}


RosbagV2Writer::AbortResult RosbagV2Writer::ReleaseStagingFiles() noexcept {
  AbortResult result;
  if (parent_dir_fd_ >= 0 && !temp_name_.empty()) {
    if (::unlinkat(parent_dir_fd_, temp_name_.c_str(), 0) != 0 && errno != ENOENT) {
      MarkCleanupError(&result, ErrnoMessage("unlink temporary bag failed", errno));
    }
  }
  if (lock_fd_ >= 0) {
    if (::close(lock_fd_) != 0) {
      MarkCleanupError(&result, ErrnoMessage("close bag lock failed", errno));
    }
    lock_fd_ = -1;
  }
  if (temp_fd_ >= 0) {
    if (::close(temp_fd_) != 0) {
      MarkCleanupError(&result, ErrnoMessage("close temporary bag fd failed", errno));
    }
    temp_fd_ = -1;
  }
  if (parent_dir_fd_ >= 0 && !lock_name_.empty()) {
    if (::unlinkat(parent_dir_fd_, lock_name_.c_str(), 0) != 0 && errno != ENOENT) {
      MarkCleanupError(&result, ErrnoMessage("unlink bag lock failed", errno));
    }
  }
  if (parent_dir_fd_ >= 0) {
    if (::close(parent_dir_fd_) != 0) {
      MarkCleanupError(&result, ErrnoMessage("close bag parent directory failed", errno));
    }
    parent_dir_fd_ = -1;
  }
  temp_path_.clear();
  temp_name_.clear();
  lock_path_.clear();
  lock_name_.clear();
  final_name_.clear();
  return result;
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldBytes(const std::string& name,
                                                       const uint8_t* data,
                                                       size_t size) {
  HeaderField field;
  field.name = name;
  field.value.assign(data, data + size);
  return field;
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldU8(const std::string& name, uint8_t value) {
  return FieldBytes(name, &value, sizeof(value));
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldU32(const std::string& name, uint32_t value) {
  return FieldBytes(name, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldU64(const std::string& name, uint64_t value) {
  return FieldBytes(name, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldTime(const std::string& name,
                                                       RosbagTime value) {
  std::vector<uint8_t> encoded;
  AppendRosTime(&encoded, value);
  HeaderField field;
  field.name = name;
  field.value = std::move(encoded);
  return field;
}

RosbagV2Writer::HeaderField RosbagV2Writer::FieldString(const std::string& name,
                                                         const std::string& value) {
  HeaderField field;
  field.name = name;
  field.value.assign(value.begin(), value.end());
  return field;
}

RosbagTime RosbagTimeFromNs(uint64_t timestamp_ns) {
  RosbagTime time;
  time.sec = static_cast<uint32_t>(timestamp_ns / kNanosecondsPerSecond);
  time.nsec = static_cast<uint32_t>(timestamp_ns % kNanosecondsPerSecond);
  return time;
}

void AppendU8(std::vector<uint8_t>* out, uint8_t value) {
  AppendBytes(out, &value, sizeof(value));
}

void AppendBool(std::vector<uint8_t>* out, bool value) {
  const uint8_t encoded = value ? 1U : 0U;
  AppendBytes(out, &encoded, sizeof(encoded));
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
  AppendBytes(out, &value, sizeof(value));
}

void AppendU64(std::vector<uint8_t>* out, uint64_t value) {
  AppendBytes(out, &value, sizeof(value));
}

void AppendF64(std::vector<uint8_t>* out, double value) {
  AppendBytes(out, &value, sizeof(value));
}

void AppendRosTime(std::vector<uint8_t>* out, RosbagTime value) {
  AppendU32(out, value.sec);
  AppendU32(out, value.nsec);
}

void AppendRosString(std::vector<uint8_t>* out, const std::string& value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("ROS string too large");
  }
  AppendU32(out, static_cast<uint32_t>(value.size()));
  AppendBytes(out, value.data(), value.size());
}

}  // namespace robobaton_demo
