#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

extern "C" {
typedef struct icm42688_sample icm42688_sample_t;
typedef struct sc132_frame sc132_frame_t;
}

namespace robobaton_demo {

constexpr int kMaxChannels = 4;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 1088;
constexpr int kDefaultFps = 60;
constexpr long long kDefaultBps = 4000;
constexpr int kDefaultRotateDegrees = 0;
constexpr int kMountRotateDegrees = 90;
constexpr uint32_t kDefaultCameraMask = (1U << kMaxChannels) - 1U;
constexpr int kBaseRtspPort = 554;
constexpr size_t kQueueCapacity = 10;
constexpr int kDefaultDiagnosticIntervalMs = 1000;
constexpr uint64_t kDefaultFrameSetMaxSkewNs = 1000000ULL;
constexpr uint32_t kDefaultFrameSetTimeoutMs = 100;
constexpr const char* kDefaultSc132TriggerMode = "software_gpio";

extern std::atomic<bool> g_stop_requested;

struct Options {
  int channels = kMaxChannels;
  uint32_t camera_mask = kDefaultCameraMask;
  int width = kDefaultWidth;
  int height = kDefaultHeight;
  int fps = kDefaultFps;
  long long bps = kDefaultBps;
  std::string url = "/PRR";
  int rotate_degrees = kDefaultRotateDegrees;
  bool diagnostics = false;
  int diagnostic_interval_ms = kDefaultDiagnosticIntervalMs;
  uint64_t frame_set_max_skew_ns = kDefaultFrameSetMaxSkewNs;
  uint32_t frame_set_timeout_ms = kDefaultFrameSetTimeoutMs;
  std::string trigger_mode = kDefaultSc132TriggerMode;
};

// 2026-07-15 修改原因：retained SC frame 必须由可移动、不可复制的 RAII job 独占，
// 队列 drain、worker 当前帧和异常展开因此都只会 release 一次。
struct QueuedFrame {
  QueuedFrame() = default;
  ~QueuedFrame();
  QueuedFrame(const QueuedFrame&) = delete;
  QueuedFrame& operator=(const QueuedFrame&) = delete;
  QueuedFrame(QueuedFrame&& other) noexcept;
  QueuedFrame& operator=(QueuedFrame&& other) noexcept;

  void Reset() noexcept;
  explicit operator bool() const noexcept { return frame != nullptr; }

  sc132_frame_t* frame = nullptr;
  int channel = 0;
  uint64_t sequence = 0;
  uint64_t frame_id = 0;
  uint64_t group_id = 0;
  uint64_t group_timestamp_ns = 0;
  uint64_t group_max_skew_ns = 0;
  uint64_t camera_timestamp_ns = 0;
  uint64_t rtsp_timestamp_ns = 0;
  uint64_t enqueue_timestamp_ns = 0;
  const void* y_data = nullptr;
  const void* uv_data = nullptr;
  uint64_t y_size = 0;
  uint64_t uv_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t vstride = 0;
};

struct ImuConsumerOptions {
  uint32_t sample_rate_hz = 1000U;
  uint32_t count = 0U;
};

using ImuSampleObserver = void (*)(const icm42688_sample_t& sample, void* user);

// 2026-07-15 修改原因：测试与 main 共用真实 ICM C-handle adapter；函数拥有唯一
// create/start/stop/destroy 生命周期，observer 异常被 callback trampoline 截断。
int RunIcmConsumer(const ImuConsumerOptions& options, ImuSampleObserver observer,
                   void* observer_user);

uint64_t SteadyClockNowNs();
int RtspPortForChannel(int channel);
uint32_t CameraMaskFromChannelCount(int channels);
int CameraMaskPopCount(uint32_t camera_mask);
bool CameraMaskContains(uint32_t camera_mask, int camera_id);
bool IsSupportedCameraMask(uint32_t camera_mask);
int OutputWidth(const Options& options);
int OutputHeight(const Options& options);
int InternalRotateDegrees(const Options& options);

}  // namespace robobaton_demo
