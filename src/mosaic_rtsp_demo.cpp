#include <signal.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include "hb_mem_mgr.h"
#include "prrtsp_v2.h"
#include "sc132camera.h"
}

#include "cam_demo_common.h"
#include "cam_demo_config.h"
#include "frozen_system_clock.h"
#include "mosaic_nv12.h"

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

namespace {

constexpr uint32_t kDefaultMosaicFps = 30U;
constexpr uint32_t kMosaicBitrateKbps = 8000U;
constexpr uint32_t kMosaicPort = 558U;
constexpr uint32_t kMosaicQueueCapacity = 2U;
constexpr uint32_t kMosaicOperationTimeoutMs = 1000U;
constexpr const char* kMosaicPath = "/PRR";
constexpr size_t kMosaicDmaBufferCount = 3U;
constexpr int64_t kMosaicDmaBufferFlags =
    HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
    HB_MEM_USAGE_HW_VIDEO_CODEC | HB_MEM_USAGE_CACHED |
    HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
constexpr int32_t kErrorCallback = -1001;
constexpr int32_t kErrorWorker = -1002;
constexpr int32_t kErrorJoin = -1003;

struct MosaicCliOptions {
  uint32_t fps = kDefaultMosaicFps;
};

volatile sig_atomic_t g_signal_stop = 0;

void SignalHandler(int) { g_signal_stop = 1; }

uint64_t Load(const std::atomic<uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

bool IsSupportedMosaicFps(uint32_t fps) noexcept {
  switch (fps) {
    case 25U:
    case 30U:
    case 40U:
    case 50U:
    case 60U:
      return true;
    default:
      return false;
  }
}

std::string RequireCliValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

uint32_t ParseMosaicFpsValue(const std::string& text) {
  if (text.empty()) {
    throw std::invalid_argument("invalid integer for --fps");
  }
  for (char character : text) {
    if (character < '0' || character > '9') {
      throw std::invalid_argument("invalid integer for --fps");
    }
  }
  unsigned long value = 0UL;
  try {
    value = std::stoul(text);
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid integer for --fps");
  }
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("invalid integer for --fps");
  }
  const uint32_t fps = static_cast<uint32_t>(value);
  if (!IsSupportedMosaicFps(fps)) {
    throw std::invalid_argument("--fps must be one of 25, 30, 40, 50, or 60");
  }
  return fps;
}

MosaicCliOptions ParseMosaicCommandLine(int argc, char** argv) {
  MosaicCliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--fps") {
      options.fps = ParseMosaicFpsValue(RequireCliValue(argc, argv, &i, "--fps"));
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  return options;
}

// frame-set 等待窗口按 3 个帧周期收敛；高帧率验证不再用 100ms 掩盖缺路。
uint32_t MosaicFrameSetTimeoutMs(uint32_t fps) noexcept {
  if (fps == 0U) {
    return robobaton_demo::kDefaultFrameSetTimeoutMs;
  }
  return (3000U + fps - 1U) / fps;
}


class DurationStats final {
 public:
  void Observe(uint64_t duration_ns) noexcept {
    count_.fetch_add(1U, std::memory_order_relaxed);
    total_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
    uint64_t previous = max_ns_.load(std::memory_order_relaxed);
    while (previous < duration_ns &&
           !max_ns_.compare_exchange_weak(previous, duration_ns,
                                          std::memory_order_relaxed)) {
    }
  }

  double AverageMs() const noexcept {
    const uint64_t samples = count_.load(std::memory_order_relaxed);
    if (samples == 0U) {
      return 0.0;
    }
    return static_cast<double>(total_ns_.load(std::memory_order_relaxed)) /
           static_cast<double>(samples) / 1000000.0;
  }

  double MaxMs() const noexcept {
    return static_cast<double>(max_ns_.load(std::memory_order_relaxed)) / 1000000.0;
  }

 private:
  std::atomic<uint64_t> count_{0U};
  std::atomic<uint64_t> total_ns_{0U};
  std::atomic<uint64_t> max_ns_{0U};
};

struct MosaicMetrics {
  std::atomic<uint64_t> frameset_received{0U};
  std::atomic<uint64_t> groups_enqueued{0U};
  std::atomic<uint64_t> groups_sent{0U};
  std::atomic<uint64_t> groups_dropped_queue_full{0U};
  std::atomic<uint64_t> groups_dropped_invalid{0U};
  std::atomic<uint64_t> retain_failures{0U};
  std::atomic<uint64_t> copy_failures{0U};
  std::atomic<uint64_t> flush_failures{0U};
  std::atomic<uint64_t> send_failures{0U};
  std::atomic<uint64_t> fatal_callback_exceptions{0U};
  std::atomic<uint64_t> retains{0U};
  std::atomic<uint64_t> releases{0U};
  DurationStats copy_duration;
  DurationStats flush_duration;
  DurationStats send_duration;
};

bool PlaneCapacityIsValid(uint32_t stride, uint32_t rows, uint64_t capacity) noexcept {
  if (stride == 0U || rows == 0U) {
    return false;
  }
  if (stride > std::numeric_limits<uint64_t>::max() / rows) {
    return false;
  }
  return capacity >= static_cast<uint64_t>(stride) * rows;
}

// hbmem 模块必须覆盖所有 graphic buffer 的生命周期；buffer 释放后才能关闭模块。
class HbMemModule final {
 public:
  HbMemModule() {
    const int32_t result = hb_mem_module_open();
    if (result != 0) {
      throw std::runtime_error("hb_mem_module_open failed status=" +
                               std::to_string(result));
    }
    opened_ = true;
  }

  ~HbMemModule() {
    if (opened_) {
      (void)hb_mem_module_close();
    }
  }

  HbMemModule(const HbMemModule&) = delete;
  HbMemModule& operator=(const HbMemModule&) = delete;

 private:
  bool opened_ = false;
};

// 输出缓冲采用 X5 hbmem NV12 graphic buffer，CPU 写入后把虚拟/物理地址直接交给编码器。
class MosaicDmaBuffer final {
 public:
  MosaicDmaBuffer() = default;
  ~MosaicDmaBuffer() { Release(); }

  MosaicDmaBuffer(const MosaicDmaBuffer&) = delete;
  MosaicDmaBuffer& operator=(const MosaicDmaBuffer&) = delete;

  void Allocate() {
    std::memset(&buffer_, 0, sizeof(buffer_));
    const int32_t result = hb_mem_alloc_graph_buf(
        static_cast<int32_t>(robobaton_demo::kMosaicOutputWidth),
        static_cast<int32_t>(robobaton_demo::kMosaicOutputHeight), MEM_PIX_FMT_NV12,
        kMosaicDmaBufferFlags,
        static_cast<int32_t>(robobaton_demo::kMosaicOutputWidth),
        static_cast<int32_t>(robobaton_demo::kMosaicOutputHeight), &buffer_);
    if (result != 0) {
      throw std::runtime_error("hb_mem_alloc_graph_buf failed status=" +
                               std::to_string(result));
    }
    allocated_ = true;
    if (!IsValid()) {
      Release();
      throw std::runtime_error("hb_mem_alloc_graph_buf returned invalid NV12 mosaic buffer");
    }
  }

  robobaton_demo::MutableNv12ImageView MutableView() noexcept {
    return robobaton_demo::MutableNv12ImageView{buffer_.virt_addr[0],
                                buffer_.virt_addr[1],
                                robobaton_demo::kMosaicOutputWidth,
                                robobaton_demo::kMosaicOutputHeight,
                                static_cast<uint32_t>(buffer_.stride),
                                static_cast<uint32_t>(buffer_.vstride),
                                buffer_.size[0],
                                buffer_.size[1]};
  }

  int32_t FlushForEncoder() noexcept {
    // 连续 graphic buffer 可能只暴露 fd[0]；按平面虚拟地址 flush，避免依赖 UV fd。
    const int32_t y_result = hb_mem_flush_buf_with_vaddr(
        reinterpret_cast<uint64_t>(buffer_.virt_addr[0]), buffer_.size[0]);
    if (y_result != 0) {
      return y_result;
    }
    return hb_mem_flush_buf_with_vaddr(
        reinterpret_cast<uint64_t>(buffer_.virt_addr[1]), buffer_.size[1]);
  }

  uint64_t y_virtual_address() const noexcept {
    return reinterpret_cast<uint64_t>(buffer_.virt_addr[0]);
  }
  uint64_t uv_virtual_address() const noexcept {
    return reinterpret_cast<uint64_t>(buffer_.virt_addr[1]);
  }
  uint64_t y_physical_address() const noexcept { return buffer_.phys_addr[0]; }
  uint64_t uv_physical_address() const noexcept { return buffer_.phys_addr[1]; }
  uint64_t y_size_bytes() const noexcept { return buffer_.size[0]; }
  uint64_t uv_size_bytes() const noexcept { return buffer_.size[1]; }
  uint32_t stride() const noexcept { return static_cast<uint32_t>(buffer_.stride); }
  uint32_t vstride() const noexcept { return static_cast<uint32_t>(buffer_.vstride); }

 private:
  bool IsValid() const noexcept {
    if (!allocated_ || buffer_.plane_cnt < 2 || buffer_.format != MEM_PIX_FMT_NV12 ||
        buffer_.width != static_cast<int32_t>(robobaton_demo::kMosaicOutputWidth) ||
        buffer_.height != static_cast<int32_t>(robobaton_demo::kMosaicOutputHeight) ||
        buffer_.stride <= 0 || buffer_.vstride <= 0 || (buffer_.vstride & 1) != 0 ||
        buffer_.fd[0] < 0 || buffer_.virt_addr[0] == nullptr ||
        buffer_.virt_addr[1] == nullptr || buffer_.phys_addr[0] == 0U ||
        buffer_.phys_addr[1] == 0U) {
      return false;
    }
    const uint32_t stride = static_cast<uint32_t>(buffer_.stride);
    const uint32_t vstride = static_cast<uint32_t>(buffer_.vstride);
    if (stride < robobaton_demo::kMosaicOutputWidth ||
        vstride < robobaton_demo::kMosaicOutputHeight) {
      return false;
    }
    return PlaneCapacityIsValid(stride, vstride, buffer_.size[0]) &&
           PlaneCapacityIsValid(stride, vstride / 2U, buffer_.size[1]) &&
           buffer_.size[0] <= std::numeric_limits<uint32_t>::max() &&
           buffer_.size[1] <= std::numeric_limits<uint32_t>::max() &&
           buffer_.size[0] <= std::numeric_limits<uint32_t>::max() - buffer_.size[1];
  }

  void Release() noexcept {
    if (allocated_) {
      (void)hb_mem_free_buf(buffer_.fd[0]);
      allocated_ = false;
    }
    std::memset(&buffer_, 0, sizeof(buffer_));
  }

  hb_mem_graphic_buf_t buffer_{};
  bool allocated_ = false;
};

class MosaicDmaBufferPool;

struct MosaicDmaBufferSlot {
  MosaicDmaBuffer buffer;
  MosaicDmaBufferPool* owner = nullptr;
  size_t index = 0U;
};

// PRRTSP external NV12 回调归还 slot；pool 避免编码器短暂持有旧帧时阻塞采集处理。
class MosaicDmaBufferPool final {
 public:
  MosaicDmaBufferPool() {
    for (size_t index = 0U; index < slots_.size(); ++index) {
      slots_[index].owner = this;
      slots_[index].index = index;
      slots_[index].buffer.Allocate();
      available_[index] = true;
    }
  }

  MosaicDmaBufferPool(const MosaicDmaBufferPool&) = delete;
  MosaicDmaBufferPool& operator=(const MosaicDmaBufferPool&) = delete;

  MosaicDmaBufferSlot* Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stopping_ || HasAvailableLocked(); });
    if (stopping_) {
      return nullptr;
    }
    for (size_t index = 0U; index < available_.size(); ++index) {
      if (available_[index]) {
        available_[index] = false;
        return &slots_[index];
      }
    }
    return nullptr;
  }

  void Release(size_t index) noexcept {
    if (index >= available_.size()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      available_[index] = true;
    }
    cv_.notify_one();
  }

  void Stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
  }

 private:
  bool HasAvailableLocked() const noexcept {
    for (bool available : available_) {
      if (available) {
        return true;
      }
    }
    return false;
  }

  HbMemModule module_;
  std::array<MosaicDmaBufferSlot, kMosaicDmaBufferCount> slots_{};
  std::array<bool, kMosaicDmaBufferCount> available_{};
  bool stopping_ = false;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// 回调只修改 pool 状态，不访问帧内容；PRRTSP 保证编码器不再读取该 slot 后才调用。
void ReleaseMosaicDmaBuffer(void* user) noexcept {
  auto* slot = static_cast<MosaicDmaBufferSlot*>(user);
  if (slot != nullptr && slot->owner != nullptr) {
    slot->owner->Release(slot->index);
  }
}

struct RetainedFrameSetJob {
  uint64_t group_id = 0U;
  uint64_t timestamp_ns = 0U;
  std::array<sc132_frame_t*, robobaton_demo::kMosaicCameraCount> frames{};
};

class MosaicRtspStream final {
 public:
  MosaicRtspStream() = default;
  ~MosaicRtspStream() { (void)Close(); }

  MosaicRtspStream(const MosaicRtspStream&) = delete;
  MosaicRtspStream& operator=(const MosaicRtspStream&) = delete;

  int32_t Open(uint32_t fps) noexcept {
    prrtsp_stream_config_v2 config{};
    config.struct_size = PRRTSP_STREAM_CONFIG_V2_1_SIZE;
    config.flags = PRRTSP_STREAM_FLAG_EXTERNAL_NV12;
    config.width = robobaton_demo::kMosaicOutputWidth;
    config.height = robobaton_demo::kMosaicOutputHeight;
    config.fps_num = fps;
    config.fps_den = 1U;
    config.bitrate_kbps = kMosaicBitrateKbps;
    config.rotation_clockwise = 0U;
    config.port = kMosaicPort;
    config.operation_timeout_ms = kMosaicOperationTimeoutMs;
    config.codec = PRRTSP_CODEC_H264;
    std::snprintf(config.path, sizeof(config.path), "%s", kMosaicPath);
    return prrtsp_stream_open(&config, &stream_);
  }

  int32_t Send(MosaicDmaBufferSlot* slot, uint64_t timestamp_ns) noexcept {
    if (slot == nullptr) {
      return PRRTSP_E_INVALID_ARGUMENT;
    }
    if (stream_ == nullptr) {
      ReleaseMosaicDmaBuffer(slot);
      return PRRTSP_E_STATE;
    }
    const MosaicDmaBuffer& buffer = slot->buffer;
    prrtsp_nv12_frame_v2 frame{};
    frame.struct_size = PRRTSP_NV12_FRAME_V2_0_SIZE;
    frame.flags = 0U;
    frame.width = robobaton_demo::kMosaicOutputWidth;
    frame.height = robobaton_demo::kMosaicOutputHeight;
    frame.y_stride = buffer.stride();
    frame.uv_stride = buffer.stride();
    frame.y_vstride = buffer.vstride();
    frame.uv_vstride = buffer.vstride() / 2U;
    frame.y_virtual_address = buffer.y_virtual_address();
    frame.uv_virtual_address = buffer.uv_virtual_address();
    frame.y_physical_address = buffer.y_physical_address();
    frame.uv_physical_address = buffer.uv_physical_address();
    frame.y_size_bytes = buffer.y_size_bytes();
    frame.uv_size_bytes = buffer.uv_size_bytes();
    frame.timestamp_ns = timestamp_ns;
    return prrtsp_stream_send_external(stream_, &frame, ReleaseMosaicDmaBuffer, slot);
  }

  bool CaptureStatus() noexcept {
    status_ = {};
    status_.struct_size = PRRTSP_STREAM_STATUS_V2_0_SIZE;
    if (stream_ == nullptr) {
      return false;
    }
    status_result_ = prrtsp_stream_get_status(stream_, &status_);
    return status_result_ == PRRTSP_OK;
  }

  bool Close() noexcept {
    bool success = true;
    for (uint32_t attempt = 0U; stream_ != nullptr && attempt < 3U; ++attempt) {
      const int32_t result = prrtsp_stream_close(&stream_);
      if (result == PRRTSP_OK && stream_ == nullptr) {
        return success;
      }
      success = false;
    }
    return stream_ == nullptr && success;
  }

  const prrtsp_stream_status_v2& status() const noexcept { return status_; }
  int32_t status_result() const noexcept { return status_result_; }

 private:
  prrtsp_stream_t* stream_ = nullptr;
  prrtsp_stream_status_v2 status_{};
  int32_t status_result_ = PRRTSP_E_STATE;
};

class MosaicPipeline final {
 public:
  MosaicPipeline(MosaicRtspStream* stream, const robobaton_demo::FrozenSystemClock* clock,
                 uint32_t frame_set_timeout_ms, uint64_t frame_set_max_skew_ns)
      : stream_(stream),
        clock_(clock),
        frame_set_timeout_ms_(frame_set_timeout_ms),
        frame_set_max_skew_ns_(frame_set_max_skew_ns) {
    if (stream_ == nullptr || clock_ == nullptr || frame_set_timeout_ms_ == 0U ||
        frame_set_max_skew_ns_ == 0U) {
      throw std::invalid_argument(
          "mosaic pipeline requires valid stream, clock, and frame-set limits");
    }
  }

  MosaicPipeline(const MosaicPipeline&) = delete;
  MosaicPipeline& operator=(const MosaicPipeline&) = delete;

  void StartWorker() { worker_ = std::thread(&MosaicPipeline::WorkerLoop, this); }

  void BeginShutdown(bool request_sc_stop) noexcept {
    accepting_.store(false, std::memory_order_release);
    if (request_sc_stop && !sc_stop_requested_.exchange(true, std::memory_order_acq_rel)) {
      sc132_request_stop();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      while (queue_size_ > 0U) {
        RetainedFrameSetJob& job = queue_[queue_head_];
        ReleaseJob(&job);
        job = RetainedFrameSetJob{};
        queue_head_ = (queue_head_ + 1U) % queue_.size();
        --queue_size_;
      }
    }
    // PRRTSP 可延迟到 close 才释放外部 NV12 slot；先唤醒 Acquire，避免 Join 等待 close-only 回调。
    buffer_pool_.Stop();
    cv_.notify_all();
  }

  bool Join() noexcept {
    if (!worker_.joinable()) {
      return true;
    }
    try {
      worker_.join();
      return true;
    } catch (...) {
      RecordFailure(kErrorJoin);
      return false;
    }
  }

  int32_t FirstError() const noexcept { return first_error_.load(std::memory_order_acquire); }
  MosaicMetrics& metrics() noexcept { return metrics_; }

  sc132_frame_set_config_t MakeConfig() noexcept {
    sc132_frame_set_config_t config = SC132_FRAME_SET_CONFIG_INIT;
    config.callback = FrameSetCallback;
    config.user_data = this;
    config.camera_count = robobaton_demo::kMosaicCameraCount;
    config.width = robobaton_demo::kMosaicInputWidth;
    config.height = robobaton_demo::kMosaicInputHeight;
    config.timeout_ms = frame_set_timeout_ms_;
    config.max_skew_ns = frame_set_max_skew_ns_;
    return config;
  }

 private:
  static void FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) noexcept {
    auto* pipeline = static_cast<MosaicPipeline*>(user);
    if (pipeline == nullptr || frame_set == nullptr) {
      return;
    }
    try {
      pipeline->HandleFrameSet(*frame_set);
    } catch (...) {
      pipeline->metrics_.fatal_callback_exceptions.fetch_add(1U, std::memory_order_relaxed);
      pipeline->RequestFailure(kErrorCallback);
    }
  }

  uint64_t MapTimestamp(uint64_t raw_timestamp_ns) const noexcept {
    return clock_->MapRawNs(raw_timestamp_ns);
  }

  void HandleFrameSet(const sc132_frame_set_t& frame_set) {
    metrics_.frameset_received.fetch_add(1U, std::memory_order_relaxed);
    if (!accepting_.load(std::memory_order_acquire)) {
      return;
    }
    if (frame_set.struct_size != sizeof(frame_set) ||
        frame_set.camera_count != robobaton_demo::kMosaicCameraCount) {
      metrics_.groups_dropped_invalid.fetch_add(1U, std::memory_order_relaxed);
      throw std::runtime_error("invalid mosaic frame-set header");
    }

    RetainedFrameSetJob job;
    job.group_id = frame_set.group_id;
    job.timestamp_ns = MapTimestamp(frame_set.group_timestamp_ns);
    std::array<bool, robobaton_demo::kMosaicCameraCount> seen{};
    for (uint32_t index = 0U; index < frame_set.camera_count; ++index) {
      const sc132_frame_set_item_t& item = frame_set.items[index];
      if (item.frame == nullptr || item.camera_id >= robobaton_demo::kMosaicCameraCount ||
          seen[item.camera_id] || item.width != robobaton_demo::kMosaicInputWidth ||
          item.height != robobaton_demo::kMosaicInputHeight) {
        metrics_.groups_dropped_invalid.fetch_add(1U, std::memory_order_relaxed);
        ReleaseJob(&job);
        throw std::runtime_error("invalid mosaic frame-set item");
      }
      seen[item.camera_id] = true;
      if (sc132_frame_retain(item.frame) != SC132_STATUS_OK) {
        metrics_.retain_failures.fetch_add(1U, std::memory_order_relaxed);
        ReleaseJob(&job);
        throw std::runtime_error("mosaic frame retain failed");
      }
      metrics_.retains.fetch_add(1U, std::memory_order_relaxed);
      job.frames[item.camera_id] = item.frame;
    }
    for (bool observed : seen) {
      if (!observed) {
        metrics_.groups_dropped_invalid.fetch_add(1U, std::memory_order_relaxed);
        ReleaseJob(&job);
        throw std::runtime_error("mosaic frame-set misses camera");
      }
    }

    if (!PushJob(&job)) {
      metrics_.groups_dropped_queue_full.fetch_add(1U, std::memory_order_relaxed);
      ReleaseJob(&job);
      return;
    }
    metrics_.groups_enqueued.fetch_add(1U, std::memory_order_relaxed);
  }

  bool PushJob(RetainedFrameSetJob* job) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_size_ >= queue_.size()) {
      return false;
    }
    const size_t tail = (queue_head_ + queue_size_) % queue_.size();
    queue_[tail] = *job;
    *job = RetainedFrameSetJob{};
    ++queue_size_;
    cv_.notify_one();
    return true;
  }

  bool PopJob(RetainedFrameSetJob* job) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stopping_ || queue_size_ > 0U; });
    if (queue_size_ == 0U) {
      return false;
    }
    *job = queue_[queue_head_];
    queue_[queue_head_] = RetainedFrameSetJob{};
    queue_head_ = (queue_head_ + 1U) % queue_.size();
    --queue_size_;
    return true;
  }

  void WorkerLoop() noexcept {
    try {
      while (true) {
        RetainedFrameSetJob job;
        if (!PopJob(&job)) {
          break;
        }
        try {
          if (!ProcessJob(job)) {
            ReleaseJob(&job);
            break;
          }
        } catch (...) {
          ReleaseJob(&job);
          throw;
        }
        ReleaseJob(&job);
      }
    } catch (...) {
      RequestFailure(kErrorWorker);
    }
  }

  // 每个 frame-set 只申请一个输出 slot；任何异常路径必须在抛出前归还 slot。
  bool ProcessJob(const RetainedFrameSetJob& job) {
    MosaicDmaBufferSlot* output_slot = buffer_pool_.Acquire();
    if (output_slot == nullptr) {
      return false;
    }
    std::array<robobaton_demo::Nv12ImageView, robobaton_demo::kMosaicCameraCount> sources{};
    const auto copy_start = std::chrono::steady_clock::now();
    for (uint32_t camera = 0U; camera < robobaton_demo::kMosaicCameraCount; ++camera) {
      sc132_frame_info_t info{};
      info.struct_size = sizeof(info);
      if (sc132_frame_get_info(job.frames[camera], &info) != SC132_STATUS_OK ||
          info.struct_size != sizeof(info) || info.camera_id != camera) {
        buffer_pool_.Release(output_slot->index);
        throw std::runtime_error("sc132_frame_get_info failed for mosaic frame");
      }
      sources[camera] = robobaton_demo::Nv12ImageView{
          static_cast<const uint8_t*>(info.y_data),
          static_cast<const uint8_t*>(info.uv_data),
          info.width,
          info.height,
          info.stride,
          info.vstride,
          info.y_size,
          info.uv_size};
    }
    const robobaton_demo::MutableNv12ImageView destination =
        output_slot->buffer.MutableView();
    const robobaton_demo::MosaicNv12Status copy_status =
        robobaton_demo::CopyNv12Mosaic2x2(sources, destination);
    const auto copy_end = std::chrono::steady_clock::now();
    metrics_.copy_duration.Observe(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(copy_end - copy_start).count()));
    if (copy_status != robobaton_demo::MosaicNv12Status::kOk) {
      metrics_.copy_failures.fetch_add(1U, std::memory_order_relaxed);
      buffer_pool_.Release(output_slot->index);
      throw std::runtime_error(std::string("mosaic copy failed: ") +
                               robobaton_demo::MosaicNv12StatusName(copy_status));
    }

    const auto flush_start = std::chrono::steady_clock::now();
    const int32_t flush_result = output_slot->buffer.FlushForEncoder();
    const auto flush_end = std::chrono::steady_clock::now();
    metrics_.flush_duration.Observe(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(flush_end - flush_start).count()));
    if (flush_result != 0) {
      metrics_.flush_failures.fetch_add(1U, std::memory_order_relaxed);
      buffer_pool_.Release(output_slot->index);
      throw std::runtime_error("mosaic DMA flush failed status=" +
                               std::to_string(flush_result));
    }

    const auto send_start = std::chrono::steady_clock::now();
    const int32_t send_result = stream_->Send(output_slot, job.timestamp_ns);
    const auto send_end = std::chrono::steady_clock::now();
    metrics_.send_duration.Observe(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(send_end - send_start).count()));
    if (send_result == PRRTSP_OK) {
      metrics_.groups_sent.fetch_add(1U, std::memory_order_relaxed);
    } else {
      metrics_.send_failures.fetch_add(1U, std::memory_order_relaxed);
      throw std::runtime_error("mosaic RTSP send failed status=" + std::to_string(send_result));
    }
    return true;
  }

  void ReleaseJob(RetainedFrameSetJob* job) noexcept {
    for (sc132_frame_t*& frame : job->frames) {
      if (frame != nullptr) {
        sc132_frame_release(frame);
        metrics_.releases.fetch_add(1U, std::memory_order_relaxed);
        frame = nullptr;
      }
    }
  }

  void RecordFailure(int32_t error) noexcept {
    int32_t expected = 0;
    first_error_.compare_exchange_strong(expected, error, std::memory_order_acq_rel);
    robobaton_demo::g_stop_requested.store(true, std::memory_order_release);
  }

  void RequestFailure(int32_t error) noexcept {
    RecordFailure(error);
    BeginShutdown(true);
  }

  MosaicRtspStream* stream_ = nullptr;
  const robobaton_demo::FrozenSystemClock* clock_ = nullptr;
  uint32_t frame_set_timeout_ms_ = robobaton_demo::kDefaultFrameSetTimeoutMs;
  uint64_t frame_set_max_skew_ns_ = robobaton_demo::kDefaultFrameSetMaxSkewNs;
  MosaicMetrics metrics_;
  MosaicDmaBufferPool buffer_pool_;
  std::array<RetainedFrameSetJob, kMosaicQueueCapacity> queue_{};
  size_t queue_head_ = 0U;
  size_t queue_size_ = 0U;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> accepting_{true};
  std::atomic<bool> sc_stop_requested_{false};
  std::atomic<int32_t> first_error_{0};
  bool stopping_ = false;
};

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [--fps <25|30|40|50|60>|--version|--help]\n"
            << "Stream: four SC132 1280x1088 NV12 cameras -> "
            << robobaton_demo::kMosaicOutputWidth << "x" << robobaton_demo::kMosaicOutputHeight
            << " H.264 " << kMosaicBitrateKbps << "kbps default " << kDefaultMosaicFps
            << "fps rtsp://<board>:" << kMosaicPort << kMosaicPath << "\n"
            << "  --fps <25|30|40|50|60> Camera trigger and encoder metadata fps, default "
            << kDefaultMosaicFps << "; 60 is stress-only\n";
}

void PrintMetric(const char* name, uint64_t value) {
  std::cout << name << '=' << value << '\n';
}

void PrintMetrics(const MosaicMetrics& metrics, const prrtsp_stream_status_v2& status,
                  int32_t status_result) {
  const uint64_t retains = Load(metrics.retains);
  const uint64_t releases = Load(metrics.releases);
  const int64_t balance = static_cast<int64_t>(retains) - static_cast<int64_t>(releases);
  PrintMetric("frameset_received", Load(metrics.frameset_received));
  PrintMetric("groups_enqueued", Load(metrics.groups_enqueued));
  PrintMetric("groups_sent", Load(metrics.groups_sent));
  PrintMetric("groups_dropped_queue_full", Load(metrics.groups_dropped_queue_full));
  PrintMetric("queue_full_drop", Load(metrics.groups_dropped_queue_full));
  PrintMetric("groups_dropped_invalid", Load(metrics.groups_dropped_invalid));
  PrintMetric("invalid_group", Load(metrics.groups_dropped_invalid));
  PrintMetric("retain_failure", Load(metrics.retain_failures));
  PrintMetric("copy_failure", Load(metrics.copy_failures));
  PrintMetric("flush_failure", Load(metrics.flush_failures));
  PrintMetric("send_failure", Load(metrics.send_failures));
  PrintMetric("fatal_callback_exceptions", Load(metrics.fatal_callback_exceptions));
  PrintMetric("retains", retains);
  PrintMetric("releases", releases);
  std::cout << "retain_release_balance=" << balance << '\n';
  std::cout << "copy_duration_avg_ms=" << metrics.copy_duration.AverageMs() << '\n';
  std::cout << "copy_duration_max_ms=" << metrics.copy_duration.MaxMs() << '\n';
  std::cout << "flush_duration_avg_ms=" << metrics.flush_duration.AverageMs() << '\n';
  std::cout << "flush_duration_max_ms=" << metrics.flush_duration.MaxMs() << '\n';
  std::cout << "send_duration_avg_ms=" << metrics.send_duration.AverageMs() << '\n';
  std::cout << "send_duration_max_ms=" << metrics.send_duration.MaxMs() << '\n';
  PrintMetric("prrtsp_frames_accepted", status.frames_accepted);
  PrintMetric("prrtsp_frames_failed", status.frames_failed);
  std::cout << "prrtsp_last_error=" << status.last_error << '\n';
  std::cout << "prrtsp_status_result=" << status_result << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  using namespace robobaton_demo;

  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "mosaic_rtsp_demo " << ROBOBATON_RELEASE_VERSION << "\n"
              << "libsc132 " << sc132_get_version() << " abi=" << SC132_ABI_VERSION_MAJOR
              << "." << SC132_ABI_VERSION_MINOR << "\n"
              << "libprrtsp " << prrtsp_get_version() << " abi=2.0\n";
    return 0;
  }
  if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    PrintUsage(argv[0]);
    return 0;
  }

  MosaicCliOptions cli_options;
  try {
    cli_options = ParseMosaicCommandLine(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << "\n";
    PrintUsage(argv[0]);
    return 2;
  }

  int exit_code = 0;
  bool sc_start_attempted = false;
  bool cleanup_success = true;
  std::unique_ptr<FrozenSystemClock> system_clock;
  std::unique_ptr<MosaicPipeline> pipeline;
  MosaicRtspStream stream;

  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    g_stop_requested.store(false, std::memory_order_release);

    Options options;
    options.fps = static_cast<int>(cli_options.fps);
    options.bps = kMosaicBitrateKbps;
    options.url = kMosaicPath;
    options.camera_mask = kDefaultCameraMask;
    options.channels = kMaxChannels;
    options.rotate_degrees = 0;
    options.frame_set_timeout_ms = MosaicFrameSetTimeoutMs(cli_options.fps);

    system_clock = std::make_unique<FrozenSystemClock>();
    system_clock->PrintTimeBase(std::cout);
    options.system_clock = system_clock.get();
    ConfigureSc132TriggerMode(options);
    ConfigureSc132SensorProfile(options);

    std::cout << "Starting SC132 mosaic RTSP demo camera_mask=0x" << std::hex
              << options.camera_mask << std::dec << " input_size=" << kMosaicInputWidth << "x"
              << kMosaicInputHeight << " output_size=" << kMosaicOutputWidth << "x"
              << kMosaicOutputHeight << " fps=" << options.fps << " kbps="
              << kMosaicBitrateKbps << " codec=h264 port=" << kMosaicPort
              << " path=" << kMosaicPath
              << " frame_timeout_ms=" << options.frame_set_timeout_ms << "\n";

    const int32_t open_status = stream.Open(cli_options.fps);
    if (open_status != PRRTSP_OK) {
      throw std::runtime_error("prrtsp_stream_open failed status=" +
                               std::to_string(open_status));
    }
    if (sc132_set_fps(cli_options.fps) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_fps failed");
    }
    // 输出坐标保持正装交付方向，底层使用安装补偿后的 internal rotation。
    if (sc132_set_output_rotation(
            static_cast<uint32_t>(InternalRotateDegrees(options))) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_output_rotation failed");
    }

    pipeline = std::make_unique<MosaicPipeline>(&stream, system_clock.get(),
                                                options.frame_set_timeout_ms,
                                                options.frame_set_max_skew_ns);
    pipeline->StartWorker();
    sc132_frame_set_config_t config = pipeline->MakeConfig();
    sc_start_attempted = true;
    const int32_t start_status = sc132_start_frame_set(&config, kDefaultCameraMask);
    if (start_status != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_start_frame_set failed status=" +
                               std::to_string(start_status));
    }

    while (g_signal_stop == 0 && !g_stop_requested.load(std::memory_order_acquire) &&
           pipeline->FirstError() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << "\n";
    exit_code = 1;
  } catch (...) {
    std::cerr << "fatal: unknown exception\n";
    exit_code = 1;
  }

  if (pipeline) {
    pipeline->BeginShutdown(sc_start_attempted);
    if (!pipeline->Join()) {
      std::cerr << "fatal: mosaic worker join failed\n";
      cleanup_success = false;
    }
  }
  if (!stream.CaptureStatus()) {
    std::cerr << "warning: failed to capture mosaic RTSP status\n";
  }
  if (!stream.Close()) {
    std::cerr << "fatal: mosaic RTSP close failed\n";
    cleanup_success = false;
  }
  if (sc_start_attempted) {
    sc132_stop();
    sc132_stop();
  }
  if (pipeline) {
    PrintMetrics(pipeline->metrics(), stream.status(), stream.status_result());
    if (pipeline->FirstError() != 0) {
      exit_code = 1;
    }
  }
  if (!cleanup_success) {
    exit_code = 1;
  }

  std::cout << "SC132 mosaic RTSP demo stopped exit_code=" << exit_code << "\n";
  return exit_code;
}
