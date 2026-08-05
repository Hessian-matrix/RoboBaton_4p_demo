#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace robobaton_demo {

struct RosbagTime {
  uint32_t sec = 0U;
  uint32_t nsec = 0U;
};

struct RosbagConnection {
  uint32_t id = 0U;
  std::string topic;
  std::string datatype;
  std::string md5sum;
  std::string message_definition;
};

class RosbagV2Writer final {
 public:
  RosbagV2Writer() = default;
  ~RosbagV2Writer();

  RosbagV2Writer(const RosbagV2Writer&) = delete;
  RosbagV2Writer& operator=(const RosbagV2Writer&) = delete;

  void Open(const std::string& final_path);
  uint32_t AddConnection(const std::string& topic, const std::string& datatype,
                         const std::string& md5sum,
                         const std::string& message_definition);
  void WriteMessage(uint32_t connection_id, RosbagTime time,
                    const std::vector<uint8_t>& payload);
  void Finish();
  void Abort() noexcept;

  bool IsOpen() const noexcept { return output_.is_open(); }
  const std::string& final_path() const noexcept { return final_path_; }

 private:
  struct HeaderField {
    std::string name;
    std::vector<uint8_t> value;
  };

  struct IndexEntry {
    RosbagTime time;
    uint32_t offset = 0U;
  };

  struct ChunkInfo {
    uint64_t pos = 0U;
    RosbagTime start_time;
    RosbagTime end_time;
    std::map<uint32_t, uint32_t> connection_counts;
  };

  static constexpr uint32_t kFileHeaderLength = 4096U;
  static constexpr uint32_t kChunkThresholdBytes = 16U * 1024U * 1024U;

  void EnsureOpen() const;
  uint64_t Tell();
  void Seek(uint64_t offset);
  void WriteRaw(const void* data, size_t size);
  void WriteU32(uint32_t value);
  void WriteHeader(const std::vector<HeaderField>& fields);
  void WriteDataLength(uint32_t value);
  void WriteFileHeaderRecord();
  void StartChunk(RosbagTime time);
  void StopChunk();
  void WriteChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size);
  void WriteIndexRecords();
  void WriteConnectionRecords();
  void WriteConnectionRecord(const RosbagConnection& connection);
  void WriteChunkInfoRecords();
  RosbagTime NormalizeMessageTime(RosbagTime time) noexcept;
  uint32_t CurrentChunkOffset();
  void ResetStagingState() noexcept;
  void ReleaseStagingFiles() noexcept;

  static HeaderField FieldBytes(const std::string& name, const uint8_t* data,
                                size_t size);
  static HeaderField FieldU8(const std::string& name, uint8_t value);
  static HeaderField FieldU32(const std::string& name, uint32_t value);
  static HeaderField FieldU64(const std::string& name, uint64_t value);
  static HeaderField FieldTime(const std::string& name, RosbagTime value);
  static HeaderField FieldString(const std::string& name, const std::string& value);

  std::ofstream output_;
  std::string final_path_;
  std::string temp_path_;
  std::string temp_name_;
  std::string lock_path_;
  std::string lock_name_;
  std::string final_name_;
  int parent_dir_fd_ = -1;
  int temp_fd_ = -1;
  int lock_fd_ = -1;
  uint64_t file_header_pos_ = 0U;
  uint64_t index_data_pos_ = 0U;
  bool chunk_open_ = false;
  uint64_t current_chunk_pos_ = 0U;
  uint64_t current_chunk_data_pos_ = 0U;
  ChunkInfo current_chunk_;
  std::vector<ChunkInfo> chunks_;
  std::map<uint32_t, std::vector<IndexEntry>> current_chunk_indexes_;
  std::vector<char> output_buffer_;
  std::vector<RosbagConnection> connections_;
  bool has_last_message_time_ = false;
  RosbagTime last_message_time_;
};

RosbagTime RosbagTimeFromNs(uint64_t timestamp_ns);

void AppendU8(std::vector<uint8_t>* out, uint8_t value);
void AppendBool(std::vector<uint8_t>* out, bool value);
void AppendU32(std::vector<uint8_t>* out, uint32_t value);
void AppendU64(std::vector<uint8_t>* out, uint64_t value);
void AppendF64(std::vector<uint8_t>* out, double value);
void AppendRosTime(std::vector<uint8_t>* out, RosbagTime value);
void AppendRosString(std::vector<uint8_t>* out, const std::string& value);

}  // namespace robobaton_demo
