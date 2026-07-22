#pragma once

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "sc132camera.h"

extern "C" {
typedef struct icm42688_sample icm42688_sample_t;
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
constexpr uint64_t kDefaultFrameSetMaxSkewNs = SC132_FRAME_SET_DEFAULT_MAX_SKEW_NS;
constexpr uint32_t kDefaultFrameSetTimeoutMs = 100;
constexpr const char* kDefaultSc132TriggerMode = "software_gpio";

extern std::atomic<bool> g_stop_requested;
enum class VideoCodec : uint32_t {
  kH264 = 0U,
  kH265 = 1U,
};

const char* VideoCodecName(VideoCodec codec) noexcept;

struct Options {
  int channels = kMaxChannels;
  uint32_t camera_mask = kDefaultCameraMask;
  int width = kDefaultWidth;
  int height = kDefaultHeight;
  int fps = kDefaultFps;
  long long bps = kDefaultBps;
  VideoCodec video_codec = VideoCodec::kH264;
  std::string url = "/PRR";
  int rotate_degrees = kDefaultRotateDegrees;
  bool diagnostics = false;
  int diagnostic_interval_ms = kDefaultDiagnosticIntervalMs;
  uint64_t frame_set_max_skew_ns = kDefaultFrameSetMaxSkewNs;
  uint32_t frame_set_timeout_ms = kDefaultFrameSetTimeoutMs;
  std::string trigger_mode = kDefaultSc132TriggerMode;
};

// retained SC frame 由可移动、不可复制的 RAII job 独占。
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

#ifdef RELEASE008_TESTING
void ResetImuIdleWaitCountForTest();
uint32_t ImuIdleWaitCountForTest();
#endif

using ImuSampleObserver = void (*)(const icm42688_sample_t& sample, void* user);

// adapter 独占 ICM C handle，并在 callback trampoline 截断 observer 异常。
int RunIcmConsumer(const ImuConsumerOptions& options, ImuSampleObserver observer,
                   void* observer_user);

struct ImuPrintState {
  uint32_t print_every_samples = 1U;
  uint64_t observed_samples = 0U;
  uint64_t last_timestamp_ns = 0U;
  int output_fd = STDOUT_FILENO;
  bool output_available = true;
  uint64_t dropped_output_lines = 0U;
};

// 按采样率和输出率计算抽样步长；任一输入为 0 时返回 0，表示禁用输出。
uint32_t ImuPrintEverySamples(uint32_t sample_rate_hz, uint32_t print_rate_hz);
// CLI observer 使用单次非阻塞 write；慢/关闭的输出端只丢日志，
// 消费仍覆盖每个 IMU 样本。
void PrintImuSample(const icm42688_sample_t& sample, void* user);

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
