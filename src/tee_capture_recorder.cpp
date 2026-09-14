#include "tee_capture_recorder.h"

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include "sc132camera.h"
}

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

namespace robobaton_demo {
namespace {

// 单路 raw 帧队列深度上限：writer 写盘远快于采集，上限只是防异常膨胀护栏。
constexpr std::size_t kMaxRawQueueDepth = 256U;
// 每路未配对 AU 缓冲上限；帧序上编码回调可先于对应 raw 帧入队。
constexpr std::size_t kMaxPendingAuPerCamera = 256U;
// 解码前缀字节上限；超出丢弃整个前缀，等待下一个 key frame 重建。
constexpr std::size_t kMaxPrefixBytes = 64U * 1024U * 1024U;
// 单 AU 字节上限，防御 C ABI 传入异常 size。
constexpr uint64_t kMaxEncodedFrameBytes = 4ULL * 1024ULL * 1024ULL;

std::string FormatFileIndex(uint32_t index) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%05u", index);
  return std::string(buffer);
}

// 编码帧文件扩展名随 RTSP 编码格式变化，避免 h265 流写入 .h264 文件名。
std::string EncodedExtension(VideoCodec codec) {
  return codec == VideoCodec::kH265 ? ".h265" : ".h264";
}

// 逐级创建目录；路径中已存在目录（EEXIST）视为成功。
bool MkdirAll(const std::string& path) {
  std::string current;
  for (std::size_t index = 0U; index < path.size(); ++index) {
    current.push_back(path[index]);
    if (path[index] == '/' || index + 1U == path.size()) {
      if (current == "/") {
        continue;
      }
      if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  return true;
}

void ReleaseFrame(sc132_frame_t* frame) noexcept {
  if (frame == nullptr) {
    return;
  }
  // libsc132 的 release 不返回状态码；租约数量由 retain/release 配对保证。
  sc132_frame_release(frame);
}

// writer 线程从每路队列消费的 raw 任务；帧已 retain 一次，由 writer 写盘后 release。
struct RawJob {
  sc132_frame_t* frame = nullptr;
  const void* y_data = nullptr;
  const void* uv_data = nullptr;
  uint64_t y_size = 0U;
  uint64_t uv_size = 0U;
  uint64_t group_timestamp_ns = 0U;
  uint64_t camera_timestamp_ns = 0U;
  uint64_t frame_id = 0U;
  uint64_t group_id = 0U;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
  uint32_t index = 0U;
};

struct PendingAu {
  std::vector<uint8_t> bytes;
  bool key_frame = false;
};

struct FrameEntry {
  uint32_t index = 0U;
  std::string raw_file;
  std::string au_file;
  uint64_t group_timestamp_ns = 0U;
  uint64_t camera_timestamp_ns = 0U;
  uint64_t frame_id = 0U;
  uint64_t group_id = 0U;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
  uint64_t y_size = 0U;
  uint64_t uv_size = 0U;
  uint64_t au_bytes = 0U;
  bool key_frame = false;
};

// 编码偶发滞后时 AU 晚于 raw 写盘到达：反向补配成功的 AU 先入队，
// 写盘仍由 writer 线程完成，维持编码线程零 I/O。
struct AuWriteJob {
  uint64_t frames_index = 0U;
  std::vector<uint8_t> bytes;
  bool key_frame = false;
};

struct CameraState {
  std::mutex raw_mutex;
  std::deque<RawJob> raw_jobs;
  uint32_t next_raw_index = 0U;
  uint64_t raw_written = 0U;

  // encoded_mutex 保护 prefix/pending_aus/frames/window 与编码侧计数。
  std::mutex encoded_mutex;
  std::vector<uint8_t> prefix;
  std::deque<std::pair<uint64_t, PendingAu>> pending_aus;
  std::vector<FrameEntry> frames;
  bool window_started = false;
  uint64_t window_first_ts_ns = 0U;
  uint64_t aus_matched = 0U;
  uint64_t aus_unmatched = 0U;
  uint64_t aus_seen = 0U;
  // raw 已写盘但当时 AU 未到达的帧（ts → frames 索引），迟到 AU 反向补配。
  std::vector<std::pair<uint64_t, uint64_t>> unmatched_frames;
  // 反向补配成功的 AU，等待 writer 线程写盘。
  std::deque<AuWriteJob> late_au_jobs;
  // 窗口结束时仍滞留 pending 的 AU（ts 与任一 raw 帧偏差超过半帧）；
  // 记录 ts 供编码器回调时序偏移取证。
  std::vector<uint64_t> leftover_au_ts_ns;
  uint64_t prefix_aus = 0U;
  uint64_t prefix_overflow = 0U;
};

}  // namespace

class TeeCaptureRecorder::Impl {
 public:
  Impl() = default;
  ~Impl() { Abort(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void Start(const Options& options) {
    options_ = options;
    const uint64_t warmup_seconds =
        warmup_override_.load(std::memory_order_relaxed) >= 0
            ? static_cast<uint64_t>(
                  warmup_override_.load(std::memory_order_relaxed))
            : options_.capture_warmup_seconds;
    capture_deadline_ns_.store(
        SteadyClockNowNs() + warmup_seconds * 1'000'000'000ULL,
        std::memory_order_release);
    const uint64_t count_override =
        count_override_.load(std::memory_order_relaxed);
    if (count_override > 0U) {
      options_.capture_frame_count = static_cast<uint32_t>(count_override);
    }
    if (!MkdirAll(options_.capture_directory)) {
      throw std::runtime_error("create capture directory failed: " +
                               options_.capture_directory);
    }
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options_.camera_mask, camera_id)) {
        continue;
      }
      const std::string base =
          options_.capture_directory + "/cam" + std::to_string(camera_id);
      if (!MkdirAll(base + "/raw") || !MkdirAll(base + "/au")) {
        throw std::runtime_error("create capture camera directory failed: " +
                                 base);
      }
      states_[camera_id] = std::make_unique<CameraState>();
    }
    started_at_ns_ = SteadyClockNowNs();
    try {
      writer_thread_ = std::thread(&Impl::WriterEntry, this);
    } catch (...) {
      throw std::runtime_error("create capture writer thread failed");
    }
  }

  void ObserveRawFrame(int camera_id, const QueuedFrame& frame) noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels || frame.frame == nullptr) {
      return;
    }
    CameraState* state = states_[camera_id].get();
    if (state == nullptr) {
      return;
    }
    if (SteadyClockNowNs() < capture_deadline_ns_.load(std::memory_order_acquire)) {
      warmup_skipped_frames_.fetch_add(1U, std::memory_order_relaxed);
      return;
    }
    RawJob job;
    job.frame = frame.frame;
    job.y_data = frame.y_data;
    job.uv_data = frame.uv_data;
    job.y_size = frame.y_size;
    job.uv_size = frame.uv_size;
    job.group_timestamp_ns = frame.group_timestamp_ns;
    job.camera_timestamp_ns = frame.camera_timestamp_ns;
    job.frame_id = frame.frame_id;
    job.group_id = frame.group_id;
    job.width = frame.width;
    job.height = frame.height;
    job.stride = frame.stride;
    job.vstride = frame.vstride;
    {
      // 每路只由一个 pipeline worker 生产，配额与队列深度都串行判定。
      std::lock_guard<std::mutex> lock(state->raw_mutex);
      if (state->next_raw_index >= options_.capture_frame_count) {
        quota_dropped_frames_.fetch_add(1U, std::memory_order_relaxed);
        return;
      }
      if (state->raw_jobs.size() >= kMaxRawQueueDepth) {
        queue_dropped_frames_.fetch_add(1U, std::memory_order_relaxed);
        return;
      }
      job.index = state->next_raw_index++;
      // 失败路径不 retain，也就不存在归还义务。
      if (sc132_frame_retain(job.frame) != SC132_STATUS_OK) {
        retain_failures_.fetch_add(1U, std::memory_order_relaxed);
        return;
      }
      retained_frames_.fetch_add(1U, std::memory_order_relaxed);
      state->raw_jobs.push_back(std::move(job));
    }
    {
      // 首个目标帧的时间戳作为编码回调的窗口下界；跨线程用 encoded_mutex 同步。
      std::lock_guard<std::mutex> lock(state->encoded_mutex);
      if (!state->window_started) {
        state->window_started = true;
        state->window_first_ts_ns = frame.group_timestamp_ns;
      }
    }
    writer_condition_.notify_one();
  }

  void ObserveEncodedFrame(int camera_id,
                           const prrtsp_encoded_frame_v2& frame) noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels) {
      return;
    }
    CameraState* state = states_[camera_id].get();
    if (state == nullptr) {
      return;
    }
    // 与其它编码消费者的同一套 C ABI 防御：struct_size、地址、大小、标志位。
    if (frame.struct_size < PRRTSP_ENCODED_FRAME_V2_0_SIZE ||
        frame.data_address == 0U || frame.size_bytes == 0U ||
        frame.size_bytes > kMaxEncodedFrameBytes ||
        frame.data_address >
            static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max()) ||
        (frame.flags & ~PRRTSP_ENCODED_FRAME_FLAG_KEY_FRAME) != 0U) {
      return;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(frame.data_address));
    const std::size_t size = static_cast<std::size_t>(frame.size_bytes);
    const bool key_frame =
        (frame.flags & PRRTSP_ENCODED_FRAME_FLAG_KEY_FRAME) != 0U;
    aus_seen_.fetch_add(1U, std::memory_order_relaxed);
    // 编码回调与 writer 匹配共享 pending/unmatched/late_au_jobs，统一锁内操作。
    std::lock_guard<std::mutex> lock(state->encoded_mutex);
    ++state->aus_seen;
    const uint64_t half_frame_ns =
        static_cast<uint64_t>(1'000'000'000ULL) /
        (std::max<uint32_t>(options_.fps, 1U) * 2U);
    if (!state->window_started ||
        frame.timestamp_ns + half_frame_ns < state->window_first_ts_ns) {
      // 前缀路径：key frame 重置起点，保证目标帧可从最近 IDR 解码。
      if (key_frame) {
        state->prefix.clear();
      }
      if (state->prefix.size() + size > kMaxPrefixBytes) {
        state->prefix.clear();
        ++state->prefix_overflow;
        return;
      }
      state->prefix.insert(state->prefix.end(), data, data + size);
      ++state->prefix_aus;
      return;
    }
    // 反向补配：raw 写盘早于编码完成时，该帧已登记为 unmatched；AU 迟到
    // 在此直接配对，避免窗口结束时永久失配。
    auto best_u = state->unmatched_frames.end();
    uint64_t best_u_distance = half_frame_ns + 1U;
    for (auto it = state->unmatched_frames.begin();
         it != state->unmatched_frames.end(); ++it) {
      const uint64_t distance =
          frame.timestamp_ns > it->first ? frame.timestamp_ns - it->first
                                         : it->first - frame.timestamp_ns;
      if (distance <= half_frame_ns && distance < best_u_distance) {
        best_u = it;
        best_u_distance = distance;
      }
    }
    if (best_u != state->unmatched_frames.end()) {
      AuWriteJob job;
      job.frames_index = best_u->second;
      job.bytes.assign(data, data + size);
      job.key_frame = key_frame;
      state->late_au_jobs.push_back(std::move(job));
      state->unmatched_frames.erase(best_u);
      ++state->aus_matched;
      writer_condition_.notify_one();
      return;
    }
    if (state->pending_aus.size() >= kMaxPendingAuPerCamera) {
      state->pending_aus.pop_front();
      ++state->aus_unmatched;
      return;
    }
    PendingAu au;
    au.key_frame = key_frame;
    au.bytes.assign(data, data + size);
    state->pending_aus.emplace_back(frame.timestamp_ns, std::move(au));
  }

  TeeCaptureFinishResult Finish(bool session_success) noexcept {
    TeeCaptureFinishResult result;
    StopWriter();
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      std::lock_guard<std::mutex> lock(state->encoded_mutex);
      constexpr std::size_t kMaxLeftoverTsRecorded = 32U;
      // 窗口结束后到达的 quota AU 属正常滞留；只记录 ts 落在窗口内的
      // 滞留 AU，它们才是「回调 ts 与 raw ts 偏差超半帧」的取证对象。
      const uint64_t last_raw_ts =
          state->frames.empty() ? 0U : state->frames.back().group_timestamp_ns;
      for (const auto& entry : state->pending_aus) {
        if (entry.first > last_raw_ts) {
          continue;
        }
        if (state->leftover_au_ts_ns.size() < kMaxLeftoverTsRecorded) {
          state->leftover_au_ts_ns.push_back(entry.first);
        }
      }
      if (!state->frames.empty()) {
        WriteFramesJsonl(camera_id, state);
      }
      if (!state->prefix.empty()) {
        const std::string directory =
            options_.capture_directory + "/cam" + std::to_string(camera_id);
        const std::string name =
            std::string("prefix") + EncodedExtension(options_.video_codec);
        if (!WriteFileAtomically(directory, name, state->prefix.data(),
                                 state->prefix.size())) {
          SetFatal("write capture prefix failed");
        }
      }
    }
    WriteSessionJson(session_success);
    result.data_complete = fatal_.load(std::memory_order_acquire) == 0 &&
                           session_success && AllTargetsMet();
    result.cleanup_complete = true;
    if (fatal_.load(std::memory_order_acquire) != 0) {
      result.error = ErrorMessage();
    }
    return result;
  }

  void Abort() noexcept {
    StopWriter();
    // 中止路径不写任何元数据；writer 已 release 全部 retained 帧。
  }

  bool HasFatalError() const noexcept {
    return fatal_.load(std::memory_order_acquire) != 0;
  }

  std::string ErrorMessage() const {
    std::lock_guard<std::mutex> lock(fatal_mutex_);
    return fatal_message_;
  }

  TeeCaptureRecorderStats SnapshotStats() const {
    TeeCaptureRecorderStats stats;
    stats.raw_frames_retained = retained_frames_.load(std::memory_order_relaxed);
    stats.raw_retain_failures = retain_failures_.load(std::memory_order_relaxed);
    stats.raw_frames_quota_dropped =
        quota_dropped_frames_.load(std::memory_order_relaxed);
    stats.raw_frames_queue_dropped =
        queue_dropped_frames_.load(std::memory_order_relaxed);
    stats.warmup_skipped_frames =
        warmup_skipped_frames_.load(std::memory_order_relaxed);
    stats.encoded_aus_seen = aus_seen_.load(std::memory_order_relaxed);
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      std::lock_guard<std::mutex> lock(state->encoded_mutex);
      stats.raw_frames_written += state->raw_written;
      stats.raw_frames_written_by_camera[camera_id] = state->raw_written;
      stats.encoded_aus_matched += state->aus_matched;
      stats.encoded_aus_matched_by_camera[camera_id] = state->aus_matched;
      stats.encoded_aus_unmatched += state->aus_unmatched;
      stats.prefix_aus_buffered += state->prefix_aus;
      stats.prefix_overflow_events += state->prefix_overflow;
    }
    return stats;
  }

#ifdef RELEASE008_TESTING
  void SetWarmupSecondsForTest(uint32_t seconds) noexcept {
    warmup_override_.store(seconds, std::memory_order_relaxed);
  }
  void SetCaptureFrameCountForTest(uint32_t count) noexcept {
    count_override_.store(count, std::memory_order_relaxed);
  }
  void FailRawWriteForTest() noexcept {
    fail_raw_write_.store(true, std::memory_order_release);
  }
  void FailEncodedWriteForTest() noexcept {
    fail_encoded_write_.store(true, std::memory_order_release);
  }
  uint64_t PendingRawJobsForTest(int camera_id) const noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels) {
      return 0U;
    }
    CameraState* state = states_[camera_id].get();
    if (state == nullptr) {
      return 0U;
    }
    std::lock_guard<std::mutex> lock(state->raw_mutex);
    return state->raw_jobs.size();
  }
  uint64_t PendingEncodedAusForTest(int camera_id) const noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels) {
      return 0U;
    }
    CameraState* state = states_[camera_id].get();
    if (state == nullptr) {
      return 0U;
    }
    std::lock_guard<std::mutex> lock(state->encoded_mutex);
    return state->pending_aus.size();
  }
#endif

 private:
  bool WriteFileAtomically(const std::string& directory, const std::string& name,
                           const void* data, std::size_t size) {
    const std::string tmp_path = directory + "/.tmp_" + name;
    const std::string final_path = directory + "/" + name;
    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    bool ok = true;
    if (size > 0U && std::fwrite(data, 1U, size, file) != size) {
      ok = false;
    }
    if (std::fclose(file) != 0) {
      ok = false;
    }
    if (!ok || std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
      std::remove(tmp_path.c_str());
      return false;
    }
    return true;
  }

  bool WriteCompactNv12(const RawJob& job, FILE* file) noexcept {
    const auto* y = static_cast<const uint8_t*>(job.y_data);
    const auto* uv = static_cast<const uint8_t*>(job.uv_data);
    if (y == nullptr || uv == nullptr) {
      return false;
    }
    // 逐行按 stride 截取有效 width，输出紧凑 NV12（Y 后紧跟交错 UV）。
    for (uint32_t row = 0U; row < job.height; ++row) {
      if (std::fwrite(y + static_cast<std::size_t>(row) * job.stride, 1U,
                      job.width, file) != job.width) {
        return false;
      }
    }
    const uint32_t uv_rows = job.height / 2U;
    for (uint32_t row = 0U; row < uv_rows; ++row) {
      if (std::fwrite(uv + static_cast<std::size_t>(row) * job.stride, 1U,
                      job.width, file) != job.width) {
        return false;
      }
    }
    return true;
  }

  bool AllQueuesEmpty() {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      std::lock_guard<std::mutex> lock(state->raw_mutex);
      if (!state->raw_jobs.empty()) {
        return false;
      }
    }
    return true;
  }

  void DropAllJobsForFatal() {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      std::lock_guard<std::mutex> lock(state->raw_mutex);
      while (!state->raw_jobs.empty()) {
        ReleaseFrame(state->raw_jobs.front().frame);
        state->raw_jobs.pop_front();
      }
    }
  }

  void ProcessRawJob(int camera_id, RawJob job) noexcept {
    CameraState* state = states_[camera_id].get();
    const bool fatal = fatal_.load(std::memory_order_acquire) != 0;
    const bool raw_write_fail_injected =
        fail_raw_write_.load(std::memory_order_acquire);
    bool raw_ok = false;
    if (!fatal && !raw_write_fail_injected) {
      const std::string directory =
          options_.capture_directory + "/cam" + std::to_string(camera_id) +
          "/raw";
      const std::string name = FormatFileIndex(job.index) + ".nv12";
      const std::string tmp_path = directory + "/.tmp_" + name;
      FILE* file = std::fopen(tmp_path.c_str(), "wb");
      raw_ok = file != nullptr;
      if (raw_ok) {
        raw_ok = WriteCompactNv12(job, file);
        raw_ok = std::fclose(file) == 0 && raw_ok;
      }
      if (raw_ok) {
        raw_ok =
            std::rename(tmp_path.c_str(), (directory + "/" + name).c_str()) == 0;
      }
      if (!raw_ok) {
        // raw 写盘失败视为采集 fatal：数据完整性与测试注入语义都依赖此路径。
        SetFatal("write capture frame failed");
        std::remove(tmp_path.c_str());
      }
    } else if (raw_write_fail_injected) {
      // 测试注入的 raw 写失败同样必须触发 fatal，否则注入钩子失去观测意义。
      SetFatal("write capture frame failed");
    }
    // 无论写盘成败，writer 都归还本任务 retain 的租约。
    ReleaseFrame(job.frame);

    FrameEntry entry;
    entry.index = job.index;
    entry.raw_file =
        raw_ok ? "raw/" + FormatFileIndex(job.index) + ".nv12" : "";
    entry.group_timestamp_ns = job.group_timestamp_ns;
    entry.camera_timestamp_ns = job.camera_timestamp_ns;
    entry.frame_id = job.frame_id;
    entry.group_id = job.group_id;
    entry.width = job.width;
    entry.height = job.height;
    entry.stride = job.stride;
    entry.vstride = job.vstride;
    entry.y_size = job.y_size;
    entry.uv_size = job.uv_size;

    PendingAu matched;
    bool have_match = false;
    {
      std::lock_guard<std::mutex> lock(state->encoded_mutex);
      state->frames.push_back(entry);
      // 编码回调时间戳与 raw group 时间戳偶发偏差（编码器时序抖动）；
      // 最近邻 + 半帧容差配对，避免严格相等下 AU 滞留、同序错配。
      const uint32_t fps = std::max<uint32_t>(options_.fps, 1U);
      const uint64_t half_frame_ns =
          static_cast<uint64_t>(1'000'000'000ULL) / (fps * 2U);
      auto best = state->pending_aus.end();
      uint64_t best_distance = half_frame_ns + 1U;
      for (auto it = state->pending_aus.begin(); it != state->pending_aus.end();
           ++it) {
        const uint64_t distance =
            it->first > job.group_timestamp_ns
                ? it->first - job.group_timestamp_ns
                : job.group_timestamp_ns - it->first;
        if (distance <= half_frame_ns && distance < best_distance) {
          best = it;
          best_distance = distance;
        }
      }
      if (best != state->pending_aus.end()) {
        matched = std::move(best->second);
        state->pending_aus.erase(best);
        have_match = true;
      }
    }

    bool au_ok = false;
    const bool au_write_fail_injected =
        fail_encoded_write_.load(std::memory_order_acquire);
    if (have_match && fatal_.load(std::memory_order_acquire) == 0 &&
        !au_write_fail_injected) {
      const std::string directory =
          options_.capture_directory + "/cam" + std::to_string(camera_id) +
          "/au";
      const std::string name =
          FormatFileIndex(job.index) + EncodedExtension(options_.video_codec);
      au_ok = WriteFileAtomically(directory, name, matched.bytes.data(),
                                  matched.bytes.size());
      if (!au_ok) {
        SetFatal("write capture access unit failed");
      }
    } else if (au_write_fail_injected) {
      // 测试注入的 AU 写失败与真实写失败同语义：必须触发 fatal。
      SetFatal("write capture access unit failed");
    }

    {
      std::lock_guard<std::mutex> lock(state->encoded_mutex);
      FrameEntry& stored = state->frames.back();
      if (au_ok) {
        stored.au_file =
            "au/" + FormatFileIndex(job.index) +
                EncodedExtension(options_.video_codec);
        stored.au_bytes = matched.bytes.size();
        stored.key_frame = matched.key_frame;
        ++state->aus_matched;
      } else if (have_match) {
        ++state->aus_unmatched;
      } else {
        // 编码未完成、AU 尚未到达：登记 (ts, frames 索引)，迟到 AU 反向补配。
        state->unmatched_frames.emplace_back(
            job.group_timestamp_ns,
            static_cast<uint64_t>(state->frames.size() - 1U));
      }
      if (raw_ok) {
        ++state->raw_written;
      }
    }

  }

  // 处理所有相机上「AU 迟到反向补配」的写盘任务；保持编码线程零 I/O。
  void ProcessLateAuJobs() noexcept {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      for (;;) {
        AuWriteJob job;
        {
          std::lock_guard<std::mutex> lock(state->encoded_mutex);
          if (state->late_au_jobs.empty()) {
            break;
          }
          job = std::move(state->late_au_jobs.front());
          state->late_au_jobs.pop_front();
        }
        FrameEntry entry_snapshot;
        {
          std::lock_guard<std::mutex> lock(state->encoded_mutex);
          if (job.frames_index >= state->frames.size()) {
            continue;
          }
          entry_snapshot = state->frames[job.frames_index];
        }
        const std::string directory =
            options_.capture_directory + "/cam" + std::to_string(camera_id) +
            "/au";
        const std::string name =
            FormatFileIndex(entry_snapshot.index) +
            EncodedExtension(options_.video_codec);
        const bool ok = WriteFileAtomically(directory, name, job.bytes.data(),
                                            job.bytes.size());
        if (!ok) {
          SetFatal("write capture access unit failed");
        }
        {
          std::lock_guard<std::mutex> lock(state->encoded_mutex);
          if (job.frames_index < state->frames.size()) {
            FrameEntry& stored = state->frames[job.frames_index];
            if (ok) {
              stored.au_file = "au/" + name;
              stored.au_bytes = job.bytes.size();
              stored.key_frame = job.key_frame;
            } else {
              // 写失败回滚反向补配的计数，保持统计一致。
              if (state->aus_matched > 0U) {
                --state->aus_matched;
              }
              ++state->aus_unmatched;
            }
          }
        }
      }
    }
  }

  void WriterEntry() noexcept {
    while (true) {
      RawJob job;
      int job_camera = -1;
      {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        writer_condition_.wait(lock, [this] {
          if (stopping_) {
            return true;
          }
          for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
            CameraState* state = states_[camera_id].get();
            if (state == nullptr) {
              continue;
            }
            std::lock_guard<std::mutex> guard(state->raw_mutex);
            if (!state->raw_jobs.empty()) {
              return true;
            }
            std::lock_guard<std::mutex> encoded_guard(state->encoded_mutex);
            if (!state->late_au_jobs.empty()) {
              return true;
            }
          }
          return false;
        });
        if (stopping_) {
          if (fatal_.load(std::memory_order_acquire) != 0) {
            DropAllJobsForFatal();
            break;
          }
          ProcessLateAuJobs();
          if (AllQueuesEmpty()) {
            break;
          }
        }
        for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
          CameraState* state = states_[camera_id].get();
          if (state == nullptr) {
            continue;
          }
          std::lock_guard<std::mutex> guard(state->raw_mutex);
          if (!state->raw_jobs.empty()) {
            job = std::move(state->raw_jobs.front());
            state->raw_jobs.pop_front();
            job_camera = camera_id;
            break;
          }
        }
      }
      if (job_camera < 0) {
        ProcessLateAuJobs();
        continue;
      }
      ProcessRawJob(job_camera, std::move(job));
      ProcessLateAuJobs();
    }
  }

  void StopWriter() noexcept {
    if (!writer_stopped_) {
      writer_stopped_ = true;
      {
        // stopping_ 与 writer 线程共享，写侧持 writer_mutex_ 建立同步。
        std::lock_guard<std::mutex> lock(writer_mutex_);
        stopping_ = true;
      }
      writer_condition_.notify_all();
      if (writer_thread_.joinable()) {
        writer_thread_.join();
      }
    }
  }
  std::string FrameEntryToJson(const FrameEntry& entry) {
    std::ostringstream out;
    out << "{\"index\":" << entry.index << ",\"raw_file\":\"" << entry.raw_file
        << "\",\"au_file\":\"" << entry.au_file
        << "\",\"frame_id\":" << entry.frame_id
        << ",\"group_id\":" << entry.group_id
        << ",\"group_timestamp_ns\":" << entry.group_timestamp_ns
        << ",\"camera_timestamp_ns\":" << entry.camera_timestamp_ns
        << ",\"width\":" << entry.width << ",\"height\":" << entry.height
        << ",\"stride\":" << entry.stride << ",\"vstride\":" << entry.vstride
        << ",\"y_size\":" << entry.y_size << ",\"uv_size\":" << entry.uv_size
        << ",\"au_bytes\":" << entry.au_bytes
        << ",\"key_frame\":" << (entry.key_frame ? "true" : "false") << "}\n";
    return out.str();
  }

  void WriteFramesJsonl(int camera_id, CameraState* state) {
    const std::string directory =
        options_.capture_directory + "/cam" + std::to_string(camera_id);
    const std::string tmp_path = directory + "/.tmp_frames.jsonl";
    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (file == nullptr) {
      SetFatal("write capture frames index failed");
      return;
    }
    bool ok = true;
    for (const FrameEntry& entry : state->frames) {
      const std::string line = FrameEntryToJson(entry);
      if (std::fwrite(line.data(), 1U, line.size(), file) != line.size()) {
        ok = false;
        break;
      }
    }
    if (std::fclose(file) != 0) {
      ok = false;
    }
    if (!ok ||
        std::rename(tmp_path.c_str(), (directory + "/frames.jsonl").c_str()) !=
            0) {
      std::remove(tmp_path.c_str());
      SetFatal("write capture frames index failed");
    }
  }

  bool AllTargetsMet() const {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options_.camera_mask, camera_id)) {
        continue;
      }
      const CameraState* state = states_[camera_id].get();
      if (state == nullptr || state->raw_written < options_.capture_frame_count) {
        return false;
      }
    }
    return true;
  }

  void WriteSessionJson(bool session_success) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"" << kTeeCaptureSessionSchema << "\",\n"
        << "  \"version\": \"" << ROBOBATON_RELEASE_VERSION << "\",\n"
        << "  \"camera_mask\": " << options_.camera_mask << ",\n"
        << "  \"fps\": " << options_.fps << ",\n"
        << "  \"rotate_degrees\": " << options_.rotate_degrees << ",\n"
        << "  \"codec\": \"" << VideoCodecName(options_.video_codec) << "\",\n"
        << "  \"capture_frame_count\": " << options_.capture_frame_count << ",\n"
        << "  \"capture_warmup_seconds\": " << options_.capture_warmup_seconds
        << ",\n"
        << "  \"session_success\": " << (session_success ? "true" : "false")
        << ",\n"
        << "  \"data_complete\": " << (AllTargetsMet() ? "true" : "false")
        << ",\n"
        << "  \"fatal_error\": \""
        << (HasFatalError() ? ErrorMessage() : std::string()) << "\",\n"
        << "  \"started_at_ns\": " << started_at_ns_ << ",\n"
        << "  \"finished_at_ns\": " << SteadyClockNowNs() << ",\n"
        << "  \"channels\": [";
    bool first_channel = true;
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options_.camera_mask, camera_id)) {
        continue;
      }
      const CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      if (!first_channel) {
        out << ",";
      }
      first_channel = false;
      out << "\n    {\"camera_id\": " << camera_id
          << ", \"raw_frames_written\": " << state->raw_written
          << ", \"encoded_aus_seen\": " << state->aus_seen
          << ", \"encoded_aus_matched\": " << state->aus_matched
          << ", \"encoded_aus_unmatched\": " << state->aus_unmatched
          << ", \"leftover_au_ts_ns\": [";
      for (std::size_t i = 0U; i < state->leftover_au_ts_ns.size(); ++i) {
        if (i != 0U) {
          out << ", ";
        }
        out << state->leftover_au_ts_ns[i];
      }
      out << "], \"prefix_aus\": " << state->prefix_aus
          << ", \"prefix_bytes\": " << state->prefix.size() << "}";
    }
    out << "\n  ]\n}\n";
    const std::string text = out.str();
    if (!WriteFileAtomically(options_.capture_directory, "session.json",
                             text.data(), text.size())) {
      SetFatal("write capture session metadata failed");
    }
  }

  void SetFatal(const std::string& message) noexcept {
    int expected = 0;
    if (fatal_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(fatal_mutex_);
      fatal_message_ = message;
    }
  }

  Options options_;
  std::array<std::unique_ptr<CameraState>, kMaxChannels> states_;
  std::atomic<uint64_t> capture_deadline_ns_{0U};
  uint64_t started_at_ns_ = 0U;
  std::atomic<int64_t> warmup_override_{-1};
  std::atomic<uint64_t> count_override_{0U};
  std::atomic<bool> fail_raw_write_{false};
  std::atomic<bool> fail_encoded_write_{false};

  std::mutex writer_mutex_;
  std::condition_variable writer_condition_;
  std::thread writer_thread_;
  bool stopping_ = false;
  bool writer_stopped_ = false;

  mutable std::mutex fatal_mutex_;
  std::atomic<int> fatal_{0};
  std::string fatal_message_;

  // 跨 worker/编码线程共享的全局计数；每路计数在各自 mutex 内更新。
  std::atomic<uint64_t> retained_frames_{0U};
  std::atomic<uint64_t> retain_failures_{0U};
  std::atomic<uint64_t> quota_dropped_frames_{0U};
  std::atomic<uint64_t> queue_dropped_frames_{0U};
  std::atomic<uint64_t> warmup_skipped_frames_{0U};
  std::atomic<uint64_t> aus_seen_{0U};
};

TeeCaptureRecorder::TeeCaptureRecorder() : impl_(std::make_unique<Impl>()) {}

TeeCaptureRecorder::~TeeCaptureRecorder() = default;

void TeeCaptureRecorder::Start(const Options& options) {
  impl_->Start(options);
  enabled_.store(true, std::memory_order_release);
}

void TeeCaptureRecorder::ObserveRawFrame(int camera_id,
                                         const QueuedFrame& frame) noexcept {
  if (enabled_.load(std::memory_order_acquire)) {
    impl_->ObserveRawFrame(camera_id, frame);
  }
}

void TeeCaptureRecorder::ObserveEncodedFrame(
    int camera_id, const prrtsp_encoded_frame_v2& frame) noexcept {
  if (enabled_.load(std::memory_order_acquire)) {
    impl_->ObserveEncodedFrame(camera_id, frame);
  }
}

TeeCaptureFinishResult TeeCaptureRecorder::Finish(bool session_success) noexcept {
  TeeCaptureFinishResult result;
  if (!enabled_.load(std::memory_order_acquire)) {
    result.cleanup_complete = true;
    return result;
  }
  result = impl_->Finish(session_success);
  enabled_.store(false, std::memory_order_release);
  return result;
}

void TeeCaptureRecorder::Abort() noexcept {
  if (enabled_.load(std::memory_order_acquire)) {
    impl_->Abort();
    enabled_.store(false, std::memory_order_release);
  }
}

bool TeeCaptureRecorder::HasFatalError() const noexcept {
  return impl_->HasFatalError();
}

std::string TeeCaptureRecorder::ErrorMessage() const {
  return impl_->ErrorMessage();
}

TeeCaptureRecorderStats TeeCaptureRecorder::SnapshotStats() const {
  return impl_->SnapshotStats();
}

#ifdef RELEASE008_TESTING
void TeeCaptureRecorder::SetWarmupSecondsForTest(uint32_t seconds) noexcept {
  impl_->SetWarmupSecondsForTest(seconds);
}
void TeeCaptureRecorder::SetCaptureFrameCountForTest(uint32_t count) noexcept {
  impl_->SetCaptureFrameCountForTest(count);
}
void TeeCaptureRecorder::FailRawWriteForTest() noexcept {
  impl_->FailRawWriteForTest();
}
void TeeCaptureRecorder::FailEncodedWriteForTest() noexcept {
  impl_->FailEncodedWriteForTest();
}
uint64_t TeeCaptureRecorder::PendingRawJobsForTest(int camera_id) const noexcept {
  return impl_->PendingRawJobsForTest(camera_id);
}
uint64_t TeeCaptureRecorder::PendingEncodedAusForTest(int camera_id) const noexcept {
  return impl_->PendingEncodedAusForTest(camera_id);
}
#endif

}  // namespace robobaton_demo
