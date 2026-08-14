#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
constexpr size_t kRosbagV2LatencyBucketCount = 32U;

struct RosbagV2LatencyStats {
  uint64_t count = 0U;
  uint64_t total_ns = 0U;
  uint64_t max_ns = 0U;
  std::array<uint64_t, kRosbagV2LatencyBucketCount> buckets{};
};

uint64_t RosbagV2LatencyAverageNs(const RosbagV2LatencyStats& stats) noexcept;
uint64_t RosbagV2LatencyPercentileUpperNs(const RosbagV2LatencyStats& stats,
                                          uint32_t percentile) noexcept;

struct RosbagV2WriterStats {
  RosbagV2LatencyStats chunk_open_latency;
  RosbagV2LatencyStats chunk_write_latency;
  RosbagV2LatencyStats chunk_header_patch_latency;
  RosbagV2LatencyStats chunk_index_latency;
  RosbagV2LatencyStats chunk_close_latency;
  RosbagV2LatencyStats record_write_latency;
  RosbagV2LatencyStats record_header_write_latency;
  RosbagV2LatencyStats record_payload_write_latency;
  RosbagV2LatencyStats flush_close_latency;
  uint64_t raw_write_calls = 0U;
  uint64_t raw_write_bytes = 0U;
  uint64_t record_header_write_bytes = 0U;
  uint64_t record_payload_write_bytes = 0U;
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
  enum class PublishOutcome {
    kPublishedComplete,
    kPublishedPartial,
    kPublishedDurabilityUnproven,
  };
  struct PublishResult {
    PublishOutcome outcome = PublishOutcome::kPublishedComplete;
    bool data_complete = true;
    bool cleanup_complete = true;
    std::string session_uuid;
    std::string published_path;
    std::string quarantine_path;
    std::string error;
  };
  struct AbortResult {
    bool cleanup_complete = true;
    std::string error;
  };

  PublishResult Finish(bool data_complete = true);
  AbortResult Abort() noexcept;

  bool IsOpen() const noexcept { return temp_fd_ >= 0; }
  const std::string& final_path() const noexcept { return final_path_; }
  const std::string& partial_path() const noexcept { return partial_path_; }
  const std::string& session_uuid() const noexcept { return session_uuid_; }

  RosbagV2WriterStats SnapshotStats() const noexcept;
#ifdef RELEASE008_TESTING
  size_t HeaderScratchCapacityForTest() const noexcept { return header_scratch_.capacity(); }
  void DisableStatsTimePublishForTest() noexcept { last_stats_publish_ns_ = 0U; }
#endif

 private:
  friend class SensorBagRecorder;

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
    std::vector<uint32_t> connection_counts;
  };

  static constexpr uint32_t kFileHeaderLength = 4096U;
  static constexpr uint32_t kChunkThresholdBytes = 16U * 1024U * 1024U;

  struct BufferView {
    const void* data = nullptr;
    size_t size = 0U;
  };

  void EnsureOpen() const;
  uint64_t Tell() const noexcept;
  void WriteRaw(const void* data, size_t size);
  void WriteRawBuffers(const BufferView* buffers, size_t count);
  void FlushOutputBuffer();
  void PwriteRaw(uint64_t offset, const void* data, size_t size);
  void WriteU32(uint32_t value);
  size_t WriteHeader(const std::vector<HeaderField>& fields);
  size_t WriteMessageRecord(uint32_t connection_id, RosbagTime time,
                            const std::vector<uint8_t>& payload);
  void ObserveWriterLatency(RosbagV2LatencyStats RosbagV2WriterStats::* target,
                            uint64_t latency_ns) noexcept;
  void RecordRawWriteStats(size_t size) noexcept;
  void RecordRecordHeaderWriteStats(uint64_t latency_ns, size_t bytes) noexcept;
  void RecordRecordPayloadWriteStats(uint64_t latency_ns, size_t bytes) noexcept;
  void PublishPendingStats() noexcept;
  void PublishPendingStats(uint64_t publish_ns) noexcept;
  void MaybePublishPendingStats(uint64_t now_ns) noexcept;
  std::vector<IndexEntry>& CurrentChunkIndex(uint32_t connection_id);
  void WriteDataLength(uint32_t value);
  void SerializeFileHeaderRecord();
  void WriteFileHeaderRecord();
  void PatchFileHeaderRecord();
  void StartChunk(RosbagTime time);
  void StopChunk();
  void WriteChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size);
  void PatchChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size);
  void SerializeChunkHeader(uint32_t compressed_size, uint32_t uncompressed_size);
  void WriteIndexRecords();
  void WriteConnectionRecords();
  void WriteConnectionRecord(const RosbagConnection& connection);
  void WriteChunkInfoRecords();
  RosbagTime NormalizeMessageTime(RosbagTime time) noexcept;
  uint32_t CurrentChunkOffset();
  std::string PartialName() const;
  std::string PartialPath() const;
  std::string QuarantineName(const std::string& target_name) const;
  AbortResult ReleaseStagingFiles() noexcept;

  static HeaderField FieldBytes(const std::string& name, const uint8_t* data,
                                size_t size);
  static HeaderField FieldU8(const std::string& name, uint8_t value);
  static HeaderField FieldU32(const std::string& name, uint32_t value);
  static HeaderField FieldU64(const std::string& name, uint64_t value);
  static HeaderField FieldTime(const std::string& name, RosbagTime value);
  static HeaderField FieldString(const std::string& name, const std::string& value);

  std::string final_path_;
  std::string partial_path_;
  std::string session_uuid_;
  std::string temp_path_;
  std::string temp_name_;
  std::string lock_path_;
  std::string lock_name_;
  std::string final_name_;
  int parent_dir_fd_ = -1;
  int temp_fd_ = -1;
  std::vector<uint8_t> write_buffer_;
  size_t write_buffer_used_ = 0U;
  int lock_fd_ = -1;
  uint64_t logical_offset_ = 0U;
  uint64_t file_header_pos_ = 0U;
  uint64_t index_data_pos_ = 0U;
  bool chunk_open_ = false;
  uint64_t current_chunk_pos_ = 0U;
  uint64_t current_chunk_data_pos_ = 0U;
  ChunkInfo current_chunk_;
  std::vector<ChunkInfo> chunks_;
  std::vector<std::vector<IndexEntry>> current_chunk_indexes_;
  std::vector<uint8_t> header_scratch_;
  std::vector<uint8_t> index_scratch_;
  std::vector<size_t> connection_index_reserves_;
  std::vector<RosbagConnection> connections_;
  bool has_last_message_time_ = false;
  RosbagTime last_message_time_;
  mutable std::mutex stats_mutex_;
  RosbagV2WriterStats stats_;
  RosbagV2WriterStats pending_stats_;
  uint64_t pending_stats_updates_ = 0U;
  uint64_t pending_stats_record_count_ = 0U;
  uint64_t last_stats_publish_ns_ = 0U;
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
