#include "rosbag_v2_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
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

void AppendBytes(std::vector<uint8_t>* out, const void* data, size_t size) {
  if (out == nullptr || (data == nullptr && size != 0U)) {
    throw std::invalid_argument("AppendBytes invalid argument");
  }
  if (size == 0U) {
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
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

void FsyncFd(int fd, const char* label) {
  if (::fsync(fd) != 0) {
    ThrowErrno(std::string("fsync ") + label + " failed");
  }
}

std::string ProcFdPath(int fd) {
  return "/proc/self/fd/" + std::to_string(fd);
}

}  // namespace

RosbagV2Writer::~RosbagV2Writer() { Abort(); }

void RosbagV2Writer::Open(const std::string& final_path) {
  RequireSafeFinalBagPath(final_path);
  if (output_.is_open()) {
    throw std::logic_error("rosbag writer already open");
  }

  final_path_ = final_path;
  file_header_pos_ = 0U;
  index_data_pos_ = 0U;
  chunk_open_ = false;
  current_chunk_pos_ = 0U;
  current_chunk_data_pos_ = 0U;
  current_chunk_ = ChunkInfo{};
  chunks_.clear();
  current_chunk_indexes_.clear();
  connections_.clear();
  has_last_message_time_ = false;
  last_message_time_ = RosbagTime{};

  const std::filesystem::path final_file(final_path_);
  const std::filesystem::path parent = final_file.parent_path();
  if (parent.empty()) {
    throw std::invalid_argument("bag path has no parent directory");
  }
  final_name_ = final_file.filename().string();
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

  output_buffer_.assign(kOutputBufferBytes, '\0');
  output_.rdbuf()->pubsetbuf(output_buffer_.data(),
                             static_cast<std::streamsize>(output_buffer_.size()));
  output_.open(ProcFdPath(temp_fd_), std::ios::binary | std::ios::out | std::ios::trunc);
  if (!output_) {
    ReleaseStagingFiles();
    throw std::runtime_error("open temporary bag failed: " + temp_path_);
  }

  const char version[] = "#ROSBAG V2.0\n";
  WriteRaw(version, sizeof(version) - 1U);
  file_header_pos_ = Tell();
  WriteFileHeaderRecord();
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

  RosbagConnection connection;
  connection.id = static_cast<uint32_t>(connections_.size());
  connection.topic = topic;
  connection.datatype = datatype;
  connection.md5sum = md5sum;
  connection.message_definition = message_definition;
  connections_.push_back(std::move(connection));
  return connections_.back().id;
}

void RosbagV2Writer::WriteMessage(uint32_t connection_id, RosbagTime time,
                                  const std::vector<uint8_t>& payload) {
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
  current_chunk_indexes_[connection_id].push_back(index);
  ++current_chunk_.connection_counts[connection_id];

  WriteHeader({FieldU8("op", kOpMsgData), FieldU32("conn", connection_id),
               FieldTime("time", record_time)});
  WriteDataLength(static_cast<uint32_t>(payload.size()));
  if (!payload.empty()) {
    WriteRaw(payload.data(), payload.size());
  }

  if (record_time.sec > current_chunk_.end_time.sec ||
      (record_time.sec == current_chunk_.end_time.sec &&
       record_time.nsec > current_chunk_.end_time.nsec)) {
    current_chunk_.end_time = record_time;
  } else if (record_time.sec < current_chunk_.start_time.sec ||
             (record_time.sec == current_chunk_.start_time.sec &&
              record_time.nsec < current_chunk_.start_time.nsec)) {
    current_chunk_.start_time = record_time;
  }

  if (CurrentChunkOffset() > kChunkThresholdBytes) {
    StopChunk();
  }
}

void RosbagV2Writer::Finish() {
  EnsureOpen();
  if (chunk_open_) {
    StopChunk();
  }
  index_data_pos_ = Tell();
  WriteConnectionRecords();
  WriteChunkInfoRecords();
  Seek(file_header_pos_);
  WriteFileHeaderRecord();
  output_.flush();
  if (!output_) {
    throw std::runtime_error("flush bag failed");
  }
  output_.close();
  if (output_) {
    FsyncFd(temp_fd_, "temporary bag");
    if (::renameat(parent_dir_fd_, temp_name_.c_str(), parent_dir_fd_,
                   final_name_.c_str()) != 0) {
      ThrowErrno("publish bag failed");
    }
    FsyncFd(parent_dir_fd_, "bag parent directory");
  } else {
    throw std::runtime_error("close bag failed");
  }
  ReleaseStagingFiles();
}

void RosbagV2Writer::Abort() noexcept {
  if (output_.is_open()) {
    output_.close();
  }
  ReleaseStagingFiles();
}

void RosbagV2Writer::EnsureOpen() const {
  if (!output_.is_open()) {
    throw std::logic_error("rosbag writer is not open");
  }
}

uint64_t RosbagV2Writer::Tell() {
  const std::streampos pos = output_.tellp();
  if (pos < 0) {
    throw std::runtime_error("tellp failed");
  }
  return static_cast<uint64_t>(pos);
}

void RosbagV2Writer::Seek(uint64_t offset) {
  if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    throw std::out_of_range("bag seek offset too large");
  }
  output_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!output_) {
    throw std::runtime_error("seek bag failed");
  }
}

void RosbagV2Writer::WriteRaw(const void* data, size_t size) {
  if (data == nullptr && size != 0U) {
    throw std::invalid_argument("null bag write buffer");
  }
  output_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!output_) {
    throw std::runtime_error("write bag failed");
  }
}

void RosbagV2Writer::WriteU32(uint32_t value) { WriteRaw(&value, sizeof(value)); }

void RosbagV2Writer::WriteHeader(const std::vector<HeaderField>& fields) {
  std::vector<uint8_t> header;
  for (const HeaderField& field : fields) {
    if (field.name.empty()) {
      throw std::invalid_argument("empty rosbag header field name");
    }
    const uint32_t field_length = static_cast<uint32_t>(field.name.size() + 1U + field.value.size());
    AppendU32(&header, field_length);
    AppendBytes(&header, field.name.data(), field.name.size());
    AppendU8(&header, '=');
    if (!field.value.empty()) {
      AppendBytes(&header, field.value.data(), field.value.size());
    }
  }
  WriteU32(static_cast<uint32_t>(header.size()));
  if (!header.empty()) {
    WriteRaw(header.data(), header.size());
  }
}

void RosbagV2Writer::WriteDataLength(uint32_t value) { WriteU32(value); }

void RosbagV2Writer::WriteFileHeaderRecord() {
  const uint32_t connection_count = static_cast<uint32_t>(connections_.size());
  const uint32_t chunk_count = static_cast<uint32_t>(chunks_.size());

  std::vector<uint8_t> header;
  const std::vector<HeaderField> fields = {
      FieldU8("op", kOpFileHeader), FieldU64("index_pos", index_data_pos_),
      FieldU32("conn_count", connection_count), FieldU32("chunk_count", chunk_count)};
  for (const HeaderField& field : fields) {
    const uint32_t field_length = static_cast<uint32_t>(field.name.size() + 1U + field.value.size());
    AppendU32(&header, field_length);
    AppendBytes(&header, field.name.data(), field.name.size());
    AppendU8(&header, '=');
    AppendBytes(&header, field.value.data(), field.value.size());
  }
  if (header.size() > kFileHeaderLength) {
    throw std::runtime_error("rosbag file header exceeds reserved length");
  }

  WriteU32(static_cast<uint32_t>(header.size()));
  WriteRaw(header.data(), header.size());
  WriteDataLength(kFileHeaderLength - static_cast<uint32_t>(header.size()));
  std::vector<uint8_t> padding(kFileHeaderLength - header.size(), static_cast<uint8_t>(' '));
  if (!padding.empty()) {
    WriteRaw(padding.data(), padding.size());
  }
}

void RosbagV2Writer::StartChunk(RosbagTime time) {
  current_chunk_ = ChunkInfo{};
  current_chunk_.pos = Tell();
  current_chunk_.start_time = time;
  current_chunk_.end_time = time;
  current_chunk_pos_ = current_chunk_.pos;
  WriteChunkHeader(0U, 0U);
  current_chunk_data_pos_ = Tell();
  current_chunk_indexes_.clear();
  chunk_open_ = true;
}

void RosbagV2Writer::StopChunk() {
  if (!chunk_open_) {
    return;
  }
  const uint64_t end_of_chunk_pos = Tell();
  const uint64_t chunk_size = end_of_chunk_pos - current_chunk_data_pos_;
  if (chunk_size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("rosbag chunk too large");
  }

  Seek(current_chunk_pos_);
  WriteChunkHeader(static_cast<uint32_t>(chunk_size), static_cast<uint32_t>(chunk_size));
  Seek(end_of_chunk_pos);
  WriteIndexRecords();
  chunks_.push_back(current_chunk_);
  current_chunk_indexes_.clear();
  current_chunk_ = ChunkInfo{};
  chunk_open_ = false;
}

void RosbagV2Writer::WriteChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size) {
  WriteHeader({FieldU8("op", kOpChunk), FieldString("compression", "none"),
               FieldU32("size", uncompressed_size)});
  WriteDataLength(compressed_size);
}

void RosbagV2Writer::WriteIndexRecords() {
  for (const auto& item : current_chunk_indexes_) {
    const uint32_t connection_id = item.first;
    const std::vector<IndexEntry>& index = item.second;
    if (index.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("rosbag index too large");
    }
    const uint32_t count = static_cast<uint32_t>(index.size());
    WriteHeader({FieldU8("op", kOpIndexData), FieldU32("conn", connection_id),
                 FieldU32("ver", kIndexVersion), FieldU32("count", count)});
    WriteDataLength(count * 12U);
    for (const IndexEntry& entry : index) {
      WriteU32(entry.time.sec);
      WriteU32(entry.time.nsec);
      WriteU32(entry.offset);
    }
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
    if (chunk.connection_counts.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("rosbag chunk info too large");
    }
    const uint32_t count = static_cast<uint32_t>(chunk.connection_counts.size());
    WriteHeader({FieldU8("op", kOpChunkInfo), FieldU32("ver", kChunkInfoVersion),
                 FieldU64("chunk_pos", chunk.pos), FieldTime("start_time", chunk.start_time),
                 FieldTime("end_time", chunk.end_time), FieldU32("count", count)});
    WriteDataLength(count * 8U);
    for (const auto& item : chunk.connection_counts) {
      WriteU32(item.first);
      WriteU32(item.second);
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

void RosbagV2Writer::ResetStagingState() noexcept {
  temp_path_.clear();
  temp_name_.clear();
  lock_path_.clear();
  lock_name_.clear();
  final_name_.clear();
  temp_fd_ = -1;
  lock_fd_ = -1;
  parent_dir_fd_ = -1;
}

void RosbagV2Writer::ReleaseStagingFiles() noexcept {
  if (parent_dir_fd_ >= 0 && !temp_name_.empty()) {
    (void)::unlinkat(parent_dir_fd_, temp_name_.c_str(), 0);
  }
  if (lock_fd_ >= 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  if (temp_fd_ >= 0) {
    ::close(temp_fd_);
    temp_fd_ = -1;
  }
  if (parent_dir_fd_ >= 0 && !lock_name_.empty()) {
    (void)::unlinkat(parent_dir_fd_, lock_name_.c_str(), 0);
  }
  if (parent_dir_fd_ >= 0) {
    ::close(parent_dir_fd_);
    parent_dir_fd_ = -1;
  }
  temp_path_.clear();
  temp_name_.clear();
  lock_path_.clear();
  lock_name_.clear();
  final_name_.clear();
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
