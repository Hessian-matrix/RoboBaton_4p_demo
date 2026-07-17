#include "cam_demo_pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "cam_demo_rtsp.h"

namespace robobaton_demo {
namespace {

constexpr int32_t kErrorCallback = -1001;
constexpr int32_t kErrorWorker = -1003;
constexpr int32_t kErrorJoin = -1004;

class FrameQueue {
 public:
  FrameQueue() = default;
  FrameQueue(const FrameQueue&) = delete;
  FrameQueue& operator=(const FrameQueue&) = delete;

  bool Push(QueuedFrame&& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || frames_.size() >= kQueueCapacity) {
      return false;
    }
    frames_.emplace_back(std::move(frame));
    condition_.notify_one();
    return true;
  }

  bool Pop(QueuedFrame* frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return stopped_ || !frames_.empty(); });
    if (frames_.empty()) {
      return false;
    }
    *frame = std::move(frames_.front());
    frames_.pop_front();
    return true;
  }

  void StopAndDrain() noexcept {
    std::deque<QueuedFrame> detached;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      // 锁内只 detach；可能进入 vendor 的 frame release 在 mutex 外执行。
      detached.swap(frames_);
    }
    condition_.notify_all();
    detached.clear();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<QueuedFrame> frames_;
  bool stopped_ = false;
};

class GroupSendBarrier {
 public:
  void Configure(uint32_t mask) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    mask_ = mask;
    current_group_ = 0U;
    waiting_mask_ = 0U;
    arrived_mask_ = 0U;
    sent_mask_ = 0U;
    waiting_groups_.fill(0U);
    stopped_ = false;
  }

  bool Wait(int camera_id, uint64_t group_id) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint32_t bit = 1U << static_cast<uint32_t>(camera_id);
    waiting_groups_[camera_id] = group_id;
    waiting_mask_ |= bit;
    while (!stopped_) {
      if (current_group_ == 0U && waiting_mask_ == mask_) {
        uint64_t candidate = std::numeric_limits<uint64_t>::max();
        for (int id = 0; id < kMaxChannels; ++id) {
          if ((mask_ & (1U << static_cast<uint32_t>(id))) != 0U) {
            candidate = std::min(candidate, waiting_groups_[id]);
          }
        }
        current_group_ = candidate;
        condition_.notify_all();
      }
      if (group_id < current_group_) {
        waiting_mask_ &= ~bit;
        return false;
      }
      if (group_id == current_group_) {
        break;
      }
      condition_.wait(lock);
    }
    if (stopped_) {
      waiting_mask_ &= ~bit;
      return false;
    }

    waiting_mask_ &= ~bit;
    arrived_mask_ |= bit;
    if (arrived_mask_ == mask_) {
      condition_.notify_all();
    } else {
      condition_.wait(lock, [&] { return stopped_ || arrived_mask_ == mask_; });
    }
    return !stopped_;
  }

  void MarkSent(int camera_id, uint64_t group_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || group_id != current_group_) {
      return;
    }
    sent_mask_ |= 1U << static_cast<uint32_t>(camera_id);
    if (sent_mask_ == mask_) {
      current_group_ = 0U;
      arrived_mask_ = 0U;
      sent_mask_ = 0U;
      condition_.notify_all();
    }
  }

  void Stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::array<uint64_t, kMaxChannels> waiting_groups_{};
  uint32_t mask_ = 0U;
  uint32_t waiting_mask_ = 0U;
  uint32_t arrived_mask_ = 0U;
  uint32_t sent_mask_ = 0U;
  uint64_t current_group_ = 0U;
  bool stopped_ = false;
};

}  // namespace

class FramePipeline::Impl {
 public:
  Impl(Options options, RtspChannels* rtsp, PipelineHooks hooks)
      : options_(std::move(options)), rtsp_(rtsp), hooks_(hooks) {
    if (rtsp_ == nullptr) {
      throw std::invalid_argument("FramePipeline requires RTSP owner");
    }
    group_barrier_.Configure(options_.camera_mask);
  }

  ~Impl() = default;

  void StartWorkers() {
    try {
      for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
        if (!CameraMaskContains(options_.camera_mask, camera_id)) {
          continue;
        }
        std::function<void()> entry = [this, camera_id] { WorkerEntry(camera_id); };
        if (hooks_.create_thread != nullptr) {
          workers_[camera_id] = hooks_.create_thread(std::move(entry), hooks_.user);
        } else {
          workers_[camera_id] = std::thread(std::move(entry));
        }
        worker_owned_[camera_id].store(true, std::memory_order_release);
      }
    } catch (...) {
      // worker 创建失败只停止 consumer，不发布空闲态 SC stop 请求。
      RecordFailure(kErrorWorker);
      BeginShutdown(false);
      throw;
    }
  }

  void StartDiagnosticsIfEnabled() {
    if (!options_.diagnostics) {
      return;
    }
    try {
      diagnostics_worker_ = std::thread(&Impl::DiagnosticsEntry, this);
      diagnostics_owned_.store(true, std::memory_order_release);
    } catch (...) {
      // diagnostics 创建失败只停止 consumer，不发布空闲态 SC stop 请求。
      RecordFailure(kErrorWorker);
      BeginShutdown(false);
      throw;
    }
  }

  sc132_frame_set_config_t MakeFrameSetConfig(FramePipeline* owner) const {
    sc132_frame_set_config_t config = SC132_FRAME_SET_CONFIG_INIT;
    config.callback = FramePipeline::FrameSetCallback;
    config.user_data = owner;
    config.camera_count = static_cast<uint32_t>(CameraMaskPopCount(options_.camera_mask));
    config.width = static_cast<uint32_t>(OutputWidth(options_));
    config.height = static_cast<uint32_t>(OutputHeight(options_));
    config.timeout_ms = options_.frame_set_timeout_ms;
    config.max_skew_ns = options_.frame_set_max_skew_ns;
    return config;
  }

  void HandleFrameSet(const sc132_frame_set_t& frame_set) {
    if (!accepting_.load(std::memory_order_acquire)) {
      return;
    }
    const uint32_t expected_count =
        static_cast<uint32_t>(CameraMaskPopCount(options_.camera_mask));
    if (frame_set.struct_size != sizeof(frame_set) || frame_set.camera_count == 0U ||
        frame_set.camera_count != expected_count ||
        frame_set.camera_count > SC132_FRAME_SET_MAX_CAMERAS) {
      throw std::runtime_error("invalid SC frame-set header");
    }

    uint32_t observed_mask = 0U;
    std::array<QueuedFrame, kMaxChannels> jobs{};
    for (uint32_t index = 0; index < frame_set.camera_count; ++index) {
      const sc132_frame_set_item_t& item = frame_set.items[index];
      if (item.camera_id >= static_cast<uint32_t>(kMaxChannels) || item.frame == nullptr ||
          !CameraMaskContains(options_.camera_mask, static_cast<int>(item.camera_id))) {
        throw std::runtime_error("invalid SC frame-set item");
      }
      const uint32_t bit = 1U << item.camera_id;
      if ((observed_mask & bit) != 0U) {
        throw std::runtime_error("duplicate SC camera id");
      }
      observed_mask |= bit;

      sc132_frame_info_t info{};
      info.struct_size = sizeof(info);
      if (sc132_frame_get_info(item.frame, &info) != SC132_STATUS_OK ||
          info.struct_size != sizeof(info) || info.camera_id != item.camera_id ||
          info.sequence != item.sequence || info.frame_id != item.frame_id ||
          info.timestamp_ns != item.timestamp_ns || info.width != item.width ||
          info.height != item.height || info.y_data == nullptr || info.uv_data == nullptr) {
        throw std::runtime_error("SC frame info mismatch");
      }
      if (sc132_frame_retain(item.frame) != SC132_STATUS_OK) {
        throw std::runtime_error("SC frame retain failed");
      }

      // retain 后立即转交 move-only RAII job，异常由栈展开 release。
      QueuedFrame& job = jobs[index];
      job.frame = item.frame;
      job.channel = static_cast<int>(item.camera_id);
      job.sequence = info.sequence;
      job.frame_id = info.frame_id;
      job.group_id = frame_set.group_id;
      job.group_timestamp_ns = frame_set.group_timestamp_ns;
      job.group_max_skew_ns = frame_set.max_skew_ns;
      job.camera_timestamp_ns = info.timestamp_ns;
      job.rtsp_timestamp_ns = frame_set.group_timestamp_ns;
      job.enqueue_timestamp_ns = SteadyClockNowNs();
      job.y_data = info.y_data;
      job.uv_data = info.uv_data;
      job.y_size = info.y_size;
      job.uv_size = info.uv_size;
      job.width = info.width;
      job.height = info.height;
      job.stride = info.stride;
      job.vstride = info.vstride;
    }
    if (observed_mask != options_.camera_mask) {
      throw std::runtime_error("SC frame-set mask mismatch");
    }

    if (hooks_.on_frame_set != nullptr) {
      hooks_.on_frame_set(frame_set, hooks_.user);
    }
    for (uint32_t index = 0; index < frame_set.camera_count; ++index) {
      if (hooks_.before_queue_insert != nullptr) {
        hooks_.before_queue_insert(hooks_.user);
      }
      const int camera_id = jobs[index].channel;
      if (!queues_[camera_id].Push(std::move(jobs[index]))) {
        throw std::runtime_error("SC frame queue closed or full");
      }
    }
  }

  void RecordFailure(int32_t error) noexcept {
    if (error == 0) {
      error = kErrorWorker;
    }
    int32_t expected = 0;
    first_error_.compare_exchange_strong(expected, error, std::memory_order_acq_rel);
    g_stop_requested.store(true, std::memory_order_release);
  }

  void RequestFailure(int32_t error) noexcept {
    RecordFailure(error);
    BeginShutdown(true);
  }

  void BeginShutdown(bool request_sc_stop) noexcept {
    accepting_.store(false, std::memory_order_release);
    if (request_sc_stop &&
        !sc_stop_requested_.exchange(true, std::memory_order_acq_rel)) {
      // 非阻塞 request_stop 可由 producer callback/consumer worker 调用；blocking stop
      // 明确留给 FinishSc132Shutdown 所在 lifecycle owner。
      sc132_request_stop();
    }
    if (!shutdown_started_.exchange(true, std::memory_order_acq_rel)) {
      group_barrier_.Stop();
      for (FrameQueue& queue : queues_) {
        queue.StopAndDrain();
      }
      diagnostics_condition_.notify_all();
    }
  }

  bool Join() noexcept {
    bool success = true;
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!worker_owned_[camera_id].load(std::memory_order_acquire)) {
        continue;
      }
      bool joined = false;
      try {
        if (hooks_.join_thread != nullptr) {
          joined = hooks_.join_thread(workers_[camera_id], hooks_.user);
        } else {
          workers_[camera_id].join();
          joined = true;
        }
      } catch (const std::system_error&) {
        joined = false;
      } catch (...) {
        joined = false;
      }
      if (joined) {
        worker_owned_[camera_id].store(false, std::memory_order_release);
      } else {
        success = false;
      }
    }

    if (diagnostics_owned_.load(std::memory_order_acquire)) {
      bool joined = false;
      try {
        if (hooks_.join_thread != nullptr) {
          joined = hooks_.join_thread(diagnostics_worker_, hooks_.user);
        } else {
          diagnostics_worker_.join();
          joined = true;
        }
      } catch (...) {
        joined = false;
      }
      if (joined) {
        diagnostics_owned_.store(false, std::memory_order_release);
      } else {
        success = false;
      }
    }

    if (!success) {
      int32_t expected = 0;
      first_error_.compare_exchange_strong(expected, kErrorJoin, std::memory_order_acq_rel);
      quiescent_.store(false, std::memory_order_release);
      return false;
    }
    quiescent_.store(true, std::memory_order_release);
    return true;
  }

  bool HasJoinableThread() const noexcept {
    for (const std::thread& worker : workers_) {
      if (worker.joinable()) {
        return true;
      }
    }
    return diagnostics_worker_.joinable();
  }

  int32_t FirstError() const noexcept { return first_error_.load(std::memory_order_acquire); }

  uint64_t TotalSentFrames(int camera_id) const noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels) {
      return 0U;
    }
    return total_sent_[camera_id].load(std::memory_order_acquire);
  }

  bool IsQuiescent() const noexcept { return quiescent_.load(std::memory_order_acquire); }

#ifdef RELEASE008_TESTING
  size_t OwnedThreadCountForTesting() const noexcept {
    size_t count = 0U;
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (worker_owned_[camera_id].load(std::memory_order_acquire) ||
          workers_[camera_id].joinable()) {
        ++count;
      }
    }
    if (diagnostics_owned_.load(std::memory_order_acquire) || diagnostics_worker_.joinable()) {
      ++count;
    }
    return count;
  }
#endif

 private:
  void WorkerEntry(int camera_id) noexcept {
    try {
      while (true) {
        QueuedFrame frame;
        if (!queues_[camera_id].Pop(&frame)) {
          break;
        }
        if (hooks_.on_queued_frame != nullptr) {
          hooks_.on_queued_frame(frame, hooks_.user);
        }
        if (!group_barrier_.Wait(camera_id, frame.group_id)) {
          break;
        }
        const int32_t send_result = rtsp_->Send(camera_id, frame);
        if (send_result != PRRTSP_OK) {
          // 保留原始 send status，失败帧 release 且不计成功。
          RequestFailure(send_result);
          break;
        }
        group_barrier_.MarkSent(camera_id, frame.group_id);
        total_sent_[camera_id].fetch_add(1U, std::memory_order_acq_rel);
      }
    } catch (...) {
      RequestFailure(kErrorWorker);
    }
  }

  void DiagnosticsEntry() noexcept {
    try {
      std::unique_lock<std::mutex> lock(diagnostics_mutex_);
      while (!shutdown_started_.load(std::memory_order_acquire)) {
        diagnostics_condition_.wait_for(
            lock, std::chrono::milliseconds(options_.diagnostic_interval_ms),
            [&] { return shutdown_started_.load(std::memory_order_acquire); });
      }
    } catch (...) {
      RequestFailure(kErrorWorker);
    }
  }

  Options options_;
  RtspChannels* rtsp_;
  PipelineHooks hooks_;
  std::array<FrameQueue, kMaxChannels> queues_;
  GroupSendBarrier group_barrier_;
  std::array<std::thread, kMaxChannels> workers_;
  std::thread diagnostics_worker_;
  std::array<std::atomic<bool>, kMaxChannels> worker_owned_{};
  std::atomic<bool> diagnostics_owned_{false};
  std::array<std::atomic<uint64_t>, kMaxChannels> total_sent_{};
  std::atomic<bool> accepting_{true};
  std::atomic<bool> shutdown_started_{false};
  std::atomic<bool> sc_stop_requested_{false};
  std::atomic<bool> quiescent_{false};
  std::atomic<int32_t> first_error_{0};
  std::mutex diagnostics_mutex_;
  std::condition_variable diagnostics_condition_;
};

FramePipeline::FramePipeline(Options options, RtspChannels* rtsp, PipelineHooks hooks)
    : impl_(new Impl(std::move(options), rtsp, hooks)) {}

FramePipeline::~FramePipeline() {
  impl_->BeginShutdown(false);
  (void)impl_->Join();
  // 真正 join failure 时 std::thread ownership 仍在，普通析构返回会隐式 terminate；
  // 显式终止更清楚，也防止上层误以为可以 close/restart producer。
  if (impl_->HasJoinableThread()) {
    std::terminate();
  }
}

void FramePipeline::StartWorkers() { impl_->StartWorkers(); }
void FramePipeline::StartDiagnosticsIfEnabled() { impl_->StartDiagnosticsIfEnabled(); }
sc132_frame_set_config_t FramePipeline::MakeFrameSetConfig() {
  return impl_->MakeFrameSetConfig(this);
}
void FramePipeline::BeginShutdown(bool request_sc_stop) noexcept {
  impl_->BeginShutdown(request_sc_stop);
}
bool FramePipeline::Join() noexcept { return impl_->Join(); }
int32_t FramePipeline::FirstError() const noexcept { return impl_->FirstError(); }
uint64_t FramePipeline::TotalSentFrames(int camera_id) const noexcept {
  return impl_->TotalSentFrames(camera_id);
}
bool FramePipeline::IsQuiescent() const noexcept { return impl_->IsQuiescent(); }
#ifdef RELEASE008_TESTING
size_t FramePipeline::OwnedThreadCountForTesting() const noexcept {
  return impl_->OwnedThreadCountForTesting();
}
#endif

void FramePipeline::FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) noexcept {
  try {
    if (frame_set == nullptr || user == nullptr) {
      return;
    }
    auto* pipeline = static_cast<FramePipeline*>(user);
    pipeline->impl_->HandleFrameSet(*frame_set);
  } catch (...) {
    // 异常不得跨越 C ABI；callback 只请求停止，不执行 blocking cleanup。
    if (user != nullptr) {
      static_cast<FramePipeline*>(user)->impl_->RequestFailure(kErrorCallback);
    }
  }
}

bool FinishSc132Shutdown(FramePipeline* pipeline) noexcept {
  if (pipeline == nullptr) {
    return false;
  }
  pipeline->BeginShutdown(true);
  if (!pipeline->Join()) {
    std::cerr << "fatal: consumer join failed; preserving producer ownership\n";
    return false;
  }
  sc132_stop();
  sc132_stop();
  return true;
}

}  // namespace robobaton_demo
