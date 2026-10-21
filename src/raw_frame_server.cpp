#include "raw_frame_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
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

namespace robobaton_demo {
namespace {

// 每路队列与 ring 的深度上限；ring 覆盖标定请求到达前的预览滞后窗口。
constexpr std::size_t kMaxRawQueueDepth = 256U;
constexpr uint32_t kDefaultRingDepth = 16U;
constexpr uint32_t kMaxRingDepth = 64U;
// 单连接读写硬超时：慢客户端只延迟自身请求，不会无限占用 accept 线程。
constexpr int kSocketIoTimeoutMs = 5000;
// 请求行最大字节数：两个十进制字段加空格与换行足够，越界视为 bad request。
constexpr std::size_t kMaxRequestLineBytes = 64U;

// 数字解析必须严格消费整段文本，拒绝尾缀垃圾与越界。
bool ParseUint64Strict(const std::string& text, uint64_t* out) {
  if (text.empty() || text.size() > 20U) {
    return false;
  }
  uint64_t value = 0U;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(ch - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  *out = value;
  return true;
}

bool ParseCameraId(const std::string& text, int* out) {
  if (text.empty() || text.size() > 1U) {
    return false;
  }
  if (text.front() < '0' || text.front() > '9') {
    return false;
  }
  *out = text.front() - '0';
  return *out >= 0 && *out < kMaxChannels;
}

void ReleaseFrame(sc132_frame_t* frame) noexcept {
  if (frame != nullptr) {
    sc132_frame_release(frame);
  }
}

// 紧凑 NV12：Y 逐行按 stride 截取 width，UV 交错同样逐行按 stride 截取 width。
// 与 --capture-dir 写盘的 .nv12 布局一致，标定消费者可复用同一读取器。
void CopyCompactNv12(const void* y, const void* uv, uint32_t width,
                     uint32_t height, uint32_t stride,
                     std::vector<uint8_t>* out) {
  const auto* y_plane = static_cast<const uint8_t*>(y);
  const auto* uv_plane = static_cast<const uint8_t*>(uv);
  if (y_plane == nullptr || uv_plane == nullptr) {
    out->clear();
    return;
  }
  const uint64_t compact = static_cast<uint64_t>(width) *
                           static_cast<uint64_t>(height) * 3U / 2U;
  out->resize(static_cast<std::size_t>(compact));
  uint8_t* dst = out->data();
  for (uint32_t row = 0U; row < height; ++row) {
    std::memcpy(dst + static_cast<std::size_t>(row) * width,
                y_plane + static_cast<std::size_t>(row) * stride, width);
  }
  uint8_t* dst_uv = dst + static_cast<std::size_t>(width) * height;
  const uint32_t uv_rows = height / 2U;
  for (uint32_t row = 0U; row < uv_rows; ++row) {
    std::memcpy(dst_uv + static_cast<std::size_t>(row) * width,
                uv_plane + static_cast<std::size_t>(row) * stride, width);
  }
}

}  // namespace

// writer 线程消费的 raw 任务：帧已 retain 一次，由 writer 拷贝后 release。
struct RawJob {
  sc132_frame_t* frame = nullptr;
  const void* y_data = nullptr;
  const void* uv_data = nullptr;
  uint64_t group_timestamp_ns = 0U;
  uint64_t camera_timestamp_ns = 0U;
  uint64_t frame_id = 0U;
  uint64_t group_id = 0U;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
};

// 每路 ring 槽：紧凑 NV12 字节与配套元数据。
struct RingSlot {
  std::vector<uint8_t> compact;
  uint64_t group_timestamp_ns = 0U;
  uint64_t camera_timestamp_ns = 0U;
  uint64_t frame_id = 0U;
  uint64_t group_id = 0U;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
  uint64_t sequence = 0U;
  bool valid = false;
};

struct CameraState {
  std::mutex mutex;
  std::deque<RawJob> jobs;
  std::vector<RingSlot> ring;
  uint64_t next_sequence = 0U;
  uint64_t copied = 0U;
  uint64_t queue_dropped = 0U;
};

class RawFrameServer::Impl {
 public:
  Impl() = default;
  ~Impl() { Stop(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void Start(const Options& options) {
    options_ = options;
    ring_depth_ = ring_depth_override_.load(std::memory_order_relaxed) > 0U
                      ? ring_depth_override_.load(std::memory_order_relaxed)
                      : options_.raw_server_ring_depth;
    if (ring_depth_ == 0U || ring_depth_ > kMaxRingDepth) {
      throw std::runtime_error("invalid raw server ring depth");
    }
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options_.camera_mask, camera_id)) {
        continue;
      }
      auto state = std::make_unique<CameraState>();
      state->ring.resize(ring_depth_);
      states_[camera_id] = std::move(state);
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      throw std::runtime_error("raw server socket failed");
    }
    int reuse = 1;
    (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(options_.raw_server_port));
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      const int error = errno;
      ::close(listen_fd_);
      listen_fd_ = -1;
      throw std::runtime_error("raw server bind/listen failed: " +
                               std::string(std::strerror(error)));
    }
    stopped_ = false;
    try {
      writer_thread_ = std::thread(&Impl::WriterEntry, this);
    } catch (...) {
      stopped_.store(true, std::memory_order_release);
      ::close(listen_fd_);
      listen_fd_ = -1;
      throw std::runtime_error("create raw server writer thread failed");
    }
    try {
      accept_thread_ = std::thread(&Impl::AcceptEntry, this);
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_stopping_ = true;
      }
      writer_condition_.notify_all();
      writer_thread_.join();
      ::close(listen_fd_);
      listen_fd_ = -1;
      stopped_.store(true, std::memory_order_release);
      throw std::runtime_error("create raw server accept thread failed");
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
    RawJob job;
    job.frame = frame.frame;
    job.y_data = frame.y_data;
    job.uv_data = frame.uv_data;
    job.group_timestamp_ns = frame.group_timestamp_ns;
    job.camera_timestamp_ns = frame.camera_timestamp_ns;
    job.frame_id = frame.frame_id;
    job.group_id = frame.group_id;
    job.width = frame.width;
    job.height = frame.height;
    job.stride = frame.stride;
    job.vstride = frame.vstride;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->jobs.size() >= kMaxRawQueueDepth) {
        // 队列只防异常膨胀；丢弃最旧任务，新帧优先保持 ring 新鲜。
        ReleaseFrame(state->jobs.front().frame);
        state->jobs.pop_front();
        state->queue_dropped += 1U;
      }
      if (sc132_frame_retain(job.frame) != SC132_STATUS_OK) {
        retain_failures_.fetch_add(1U, std::memory_order_relaxed);
        return;
      }
      retained_frames_.fetch_add(1U, std::memory_order_relaxed);
      state->jobs.push_back(job);
    }
    writer_condition_.notify_one();
  }

  void Stop() noexcept {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
      return;
    }
    // 唤醒 accept poll：shutdown 使 listen fd 可读/出错，循环随即退出。
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
    }
    {
      std::lock_guard<std::mutex> lock(writer_mutex_);
      writer_stopping_ = true;
    }
    writer_condition_.notify_all();
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  bool HasFatalError() const noexcept {
    return fatal_.load(std::memory_order_acquire) != 0;
  }

  std::string ErrorMessage() const {
    std::lock_guard<std::mutex> lock(fatal_mutex_);
    return fatal_message_;
  }

  RawFrameServerStats SnapshotStats() const {
    RawFrameServerStats stats;
    stats.raw_frames_retained = retained_frames_.load(std::memory_order_relaxed);
    stats.raw_retain_failures =
        retain_failures_.load(std::memory_order_relaxed);
    stats.requests_total = requests_total_.load(std::memory_order_relaxed);
    stats.requests_matched = requests_matched_.load(std::memory_order_relaxed);
    stats.requests_no_match =
        requests_no_match_.load(std::memory_order_relaxed);
    stats.requests_latest = requests_latest_.load(std::memory_order_relaxed);
    stats.requests_bad = requests_bad_.load(std::memory_order_relaxed);
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      CameraState* state = states_[camera_id].get();
      if (state == nullptr) {
        continue;
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      stats.raw_frames_copied += state->copied;
      stats.raw_frames_queue_dropped += state->queue_dropped;
      stats.raw_frames_copied_by_camera[camera_id] = state->copied;
    }
    return stats;
  }

#ifdef RELEASE008_TESTING
  void SetRingDepthForTest(uint32_t depth) noexcept {
    ring_depth_override_.store(depth, std::memory_order_relaxed);
  }
  uint64_t PendingRawJobsForTest(int camera_id) const noexcept {
    if (camera_id < 0 || camera_id >= kMaxChannels) {
      return 0U;
    }
    CameraState* state = states_[camera_id].get();
    if (state == nullptr) {
      return 0U;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->jobs.size();
  }
  int BoundPortForTest() const noexcept {
    if (listen_fd_ < 0) {
      return -1;
    }
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getsockname(listen_fd_,
                    reinterpret_cast<sockaddr*>(&address),
                    &length) != 0) {
      return -1;
    }
    return ntohs(address.sin_port);
  }
#endif

 private:
  void SetFatal(const std::string& message) noexcept {
    int expected = 0;
    if (fatal_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(fatal_mutex_);
      fatal_message_ = message;
    }
  }

  void WriterEntry() noexcept {
    for (;;) {
      RawJob job;
      int job_camera = -1;
      {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        writer_condition_.wait(lock, [this] {
          if (writer_stopping_) {
            return true;
          }
          for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
            CameraState* state = states_[camera_id].get();
            if (state == nullptr) {
              continue;
            }
            std::lock_guard<std::mutex> guard(state->mutex);
            if (!state->jobs.empty()) {
              return true;
            }
          }
          return false;
        });
        if (writer_stopping_) {
          for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
            CameraState* state = states_[camera_id].get();
            if (state == nullptr) {
              continue;
            }
            std::lock_guard<std::mutex> guard(state->mutex);
            while (!state->jobs.empty()) {
              ReleaseFrame(state->jobs.front().frame);
              state->jobs.pop_front();
            }
          }
          break;
        }
        for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
          CameraState* state = states_[camera_id].get();
          if (state == nullptr) {
            continue;
          }
          std::lock_guard<std::mutex> guard(state->mutex);
          if (!state->jobs.empty()) {
            job = std::move(state->jobs.front());
            state->jobs.pop_front();
            job_camera = camera_id;
            break;
          }
        }
      }
      if (job_camera < 0) {
        continue;
      }
      ProcessRawJob(job_camera, std::move(job));
    }
  }

  void ProcessRawJob(int camera_id, RawJob job) noexcept {
    CameraState* state = states_[camera_id].get();
    std::vector<uint8_t> compact;
    // 直接按裸字段拷贝像素，不构造持有 frame 的 QueuedFrame：其析构会重复 release。
    CopyCompactNv12(job.y_data, job.uv_data, job.width, job.height, job.stride,
                    &compact);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      RingSlot& slot = state->ring[state->next_sequence % state->ring.size()];
      slot.compact = std::move(compact);
      slot.group_timestamp_ns = job.group_timestamp_ns;
      slot.camera_timestamp_ns = job.camera_timestamp_ns;
      slot.frame_id = job.frame_id;
      slot.group_id = job.group_id;
      slot.width = job.width;
      slot.height = job.height;
      slot.stride = job.stride;
      slot.vstride = job.vstride;
      slot.sequence = state->next_sequence;
      slot.valid = true;
      state->next_sequence += 1U;
      state->copied += 1U;
    }
    // 拷贝完成后归还租约，与写盘采集同一哲学：像素搬运不阻塞帧路径。
    ReleaseFrame(job.frame);
  }

  void AcceptEntry() noexcept {
    while (!stopped_.load(std::memory_order_acquire)) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      const int ready = ::poll(&pfd, 1U, 200);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (ready == 0) {
        continue;
      }
      int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EBADF || errno == EINVAL) {
          continue;
        }
        break;
      }
      HandleClient(client);
      ::close(client);
    }
  }

  void HandleClient(int client) noexcept {
    timeval timeout{kSocketIoTimeoutMs / 1000,
                    (kSocketIoTimeoutMs % 1000) * 1000};
    (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string line;
    char buffer[64];
    for (;;) {
      const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        return;  // 超时、对端关闭或错误：直接放弃该连接。
      }
      line.append(buffer, static_cast<std::size_t>(received));
      if (line.find('\n') != std::string::npos) {
        break;
      }
      if (line.size() > kMaxRequestLineBytes) {
        requests_bad_.fetch_add(1U, std::memory_order_relaxed);
        SendHeader(client, RawFrameResponseStatus::kBadRequest, -1, {});
        return;
      }
    }

    const std::size_t newline = line.find('\n');
    std::string request = line.substr(0U, newline);
    if (!request.empty() && request.back() == '\r') {
      request.pop_back();
    }
    requests_total_.fetch_add(1U, std::memory_order_relaxed);

    std::istringstream stream(request);
    std::string command;
    stream >> command;
    if (command == "LATEST") {
      int camera_id = -1;
      std::string camera_text;
      stream >> camera_text;
      if (!ParseCameraId(camera_text, &camera_id)) {
        requests_bad_.fetch_add(1U, std::memory_order_relaxed);
        SendHeader(client, RawFrameResponseStatus::kBadRequest, -1, {});
        return;
      }
      requests_latest_.fetch_add(1U, std::memory_order_relaxed);
      ServeLatest(client, camera_id);
      return;
    }
    if (command == "GET") {
      int camera_id = -1;
      std::string camera_text;
      std::string ts_text;
      uint64_t timestamp_ns = 0U;
      stream >> camera_text >> ts_text;
      std::string extra;
      if (!ParseCameraId(camera_text, &camera_id) ||
          !ParseUint64Strict(ts_text, &timestamp_ns) || (stream >> extra)) {
        requests_bad_.fetch_add(1U, std::memory_order_relaxed);
        SendHeader(client, RawFrameResponseStatus::kBadRequest, -1, {});
        return;
      }
      ServeByTimestamp(client, camera_id, timestamp_ns);
      return;
    }
    requests_bad_.fetch_add(1U, std::memory_order_relaxed);
    SendHeader(client, RawFrameResponseStatus::kBadRequest, -1, {});
  }

  bool CameraEnabled(int camera_id) const {
    return camera_id >= 0 && camera_id < kMaxChannels &&
           states_[camera_id] != nullptr;
  }

  // 最近邻半帧容差匹配：请求到达时编码帧已滞后数帧，ring 内的 group 时间戳
  // 是端到端同一数值（仅 ns→μs→90kHz 量化差 ≤11μs），匹配非精确相等而是最近邻。
  void ServeByTimestamp(int client, int camera_id, uint64_t timestamp_ns) noexcept {
    if (!CameraEnabled(camera_id)) {
      requests_no_match_.fetch_add(1U, std::memory_order_relaxed);
      SendHeader(client, RawFrameResponseStatus::kCameraDisabled, camera_id, {});
      return;
    }
    CameraState* state = states_[camera_id].get();
    const uint64_t half_frame_ns =
        static_cast<uint64_t>(1'000'000'000ULL) /
        (std::max<int>(options_.fps, 1) * 2U);
    RingSlot best;
    bool found = false;
    uint64_t best_distance = half_frame_ns + 1U;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      for (const RingSlot& slot : state->ring) {
        if (!slot.valid) {
          continue;
        }
        const uint64_t distance =
            slot.group_timestamp_ns > timestamp_ns
                ? slot.group_timestamp_ns - timestamp_ns
                : timestamp_ns - slot.group_timestamp_ns;
        if (distance <= half_frame_ns && distance < best_distance) {
          best = slot;
          best_distance = distance;
          found = true;
        }
      }
    }
    if (!found) {
      requests_no_match_.fetch_add(1U, std::memory_order_relaxed);
      SendHeader(client, RawFrameResponseStatus::kNoMatch, camera_id, {});
      return;
    }
    requests_matched_.fetch_add(1U, std::memory_order_relaxed);
    SendHeader(client, RawFrameResponseStatus::kOk, camera_id, &best);
  }

  void ServeLatest(int client, int camera_id) noexcept {
    if (!CameraEnabled(camera_id)) {
      SendHeader(client, RawFrameResponseStatus::kCameraDisabled, camera_id, {});
      return;
    }
    CameraState* state = states_[camera_id].get();
    RingSlot latest;
    bool found = false;
    uint64_t latest_sequence = 0U;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      for (const RingSlot& slot : state->ring) {
        if (slot.valid && (!found || slot.sequence >= latest_sequence)) {
          latest = slot;
          latest_sequence = slot.sequence;
          found = true;
        }
      }
    }
    if (!found) {
      SendHeader(client, RawFrameResponseStatus::kNoFrame, camera_id, {});
      return;
    }
    SendHeader(client, RawFrameResponseStatus::kOk, camera_id, &latest);
  }

  void SendHeader(int client, RawFrameResponseStatus status, int camera_id,
                  const RingSlot* slot) noexcept {
    RawFrameResponseHeader header{};
    header.magic = kRawFrameResponseMagic;
    header.version = kRawFrameResponseVersion;
    header.status = static_cast<int32_t>(status);
    header.camera_id = static_cast<int32_t>(camera_id);
    if (slot != nullptr && status == RawFrameResponseStatus::kOk) {
      header.group_timestamp_ns = slot->group_timestamp_ns;
      header.camera_timestamp_ns = slot->camera_timestamp_ns;
      header.frame_id = slot->frame_id;
      header.group_id = slot->group_id;
      header.width = slot->width;
      header.height = slot->height;
      header.stride = slot->stride;
      header.vstride = slot->vstride;
      header.payload_size = slot->compact.size();
    }
    if (!SendAll(client, &header, sizeof(header))) {
      return;
    }
    if (slot != nullptr && status == RawFrameResponseStatus::kOk &&
        !slot->compact.empty()) {
      (void)SendAll(client, slot->compact.data(), slot->compact.size());
    }
  }

  bool SendAll(int client, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::size_t sent = 0U;
    while (sent < size) {
      const ssize_t result =
          ::send(client, bytes + sent, size - sent, MSG_NOSIGNAL);
      if (result <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(result);
    }
    return true;
  }

  Options options_;
  std::array<std::unique_ptr<CameraState>, kMaxChannels> states_;
  std::atomic<uint32_t> ring_depth_override_{0U};
  uint32_t ring_depth_ = kDefaultRingDepth;

  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::thread writer_thread_;
  std::mutex writer_mutex_;
  std::condition_variable writer_condition_;
  bool writer_stopping_ = false;
  std::atomic<bool> stopped_{true};

  mutable std::mutex fatal_mutex_;
  std::atomic<int> fatal_{0};
  std::string fatal_message_;

  std::atomic<uint64_t> retained_frames_{0U};
  std::atomic<uint64_t> retain_failures_{0U};
  std::atomic<uint64_t> requests_total_{0U};
  std::atomic<uint64_t> requests_matched_{0U};
  std::atomic<uint64_t> requests_no_match_{0U};
  std::atomic<uint64_t> requests_latest_{0U};
  std::atomic<uint64_t> requests_bad_{0U};
};

RawFrameServer::RawFrameServer() : impl_(std::make_unique<Impl>()) {}

RawFrameServer::~RawFrameServer() = default;

void RawFrameServer::Start(const Options& options) {
  impl_->Start(options);
  enabled_.store(true, std::memory_order_release);
}

void RawFrameServer::ObserveRawFrame(int camera_id,
                                     const QueuedFrame& frame) noexcept {
  if (enabled_.load(std::memory_order_acquire)) {
    impl_->ObserveRawFrame(camera_id, frame);
  }
}

void RawFrameServer::Stop() noexcept {
  if (enabled_.load(std::memory_order_acquire)) {
    impl_->Stop();
    enabled_.store(false, std::memory_order_release);
  }
}

bool RawFrameServer::HasFatalError() const noexcept {
  return impl_->HasFatalError();
}

std::string RawFrameServer::ErrorMessage() const {
  return impl_->ErrorMessage();
}

RawFrameServerStats RawFrameServer::SnapshotStats() const {
  return impl_->SnapshotStats();
}

#ifdef RELEASE008_TESTING
void RawFrameServer::SetRingDepthForTest(uint32_t depth) noexcept {
  impl_->SetRingDepthForTest(depth);
}
uint64_t RawFrameServer::PendingRawJobsForTest(int camera_id) const noexcept {
  return impl_->PendingRawJobsForTest(camera_id);
}
int RawFrameServer::BoundPortForTest() const noexcept {
  return impl_->BoundPortForTest();
}
#endif

}  // namespace robobaton_demo
