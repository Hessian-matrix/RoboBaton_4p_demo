#pragma once

#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "sc132camera.h"
#include "frozen_system_clock.h"

extern "C" {
#include "icm42688_driver.h"
}

namespace robobaton_demo {

constexpr int kMaxChannels = 4;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 1088;
constexpr int kDefaultFps = 30;
constexpr long long kDefaultBps = 4000;
constexpr int kDefaultRotateDegrees = 0;
constexpr int kMountRotateDegrees = 90;
constexpr uint32_t kDefaultCameraMask = (1U << kMaxChannels) - 1U;
constexpr int kDefaultRtspBasePort = 554;
constexpr int kMaxRtspPort = 65535;
constexpr size_t kQueueCapacity = 10;
constexpr int kDefaultDiagnosticIntervalMs = 1000;
constexpr uint64_t kDefaultFrameSetMaxSkewNs = SC132_FRAME_SET_DEFAULT_MAX_SKEW_NS;
constexpr uint32_t kDefaultFrameSetTimeoutMs = 100;
constexpr const char* kDefaultSc132TriggerMode = "software_gpio";
constexpr uint32_t kDefaultImuSampleRateHz = 1000U;

constexpr uint32_t kDefaultImuPrintRateHz = 10U;
extern std::atomic<bool> g_stop_requested;
enum class VideoCodec : uint32_t {
  kH264 = 0U,
  kH265 = 1U,
};

enum class TimestampDomain : uint32_t {
  kUnknown = 0U,
  kMonotonicRaw = 1U,
  kSystemRealtime = 2U,
  kSc132Native = 3U,
};

enum class ImuStartOrder : uint32_t {
  kImuFirst = 0U,
  kCameraFirst = 1U,
};

struct Options;

const char* VideoCodecName(VideoCodec codec) noexcept;
const char* ImuSampleDropPolicyName(uint32_t policy) noexcept;
const char* TimestampDomainName(TimestampDomain domain) noexcept;
bool Sc132TimestampsAreMonotonicRaw(const Options& options) noexcept;
TimestampDomain Sc132OutputTimestampDomain(const Options& options) noexcept;

struct Options {
  int channels = kMaxChannels;
  uint32_t camera_mask = kDefaultCameraMask;
  int width = kDefaultWidth;
  int height = kDefaultHeight;
  int fps = kDefaultFps;
  long long bps = kDefaultBps;
  VideoCodec video_codec = VideoCodec::kH264;
  std::string url = "/PRR";
  int rtsp_base_port = kDefaultRtspBasePort;
  int rotate_degrees = kDefaultRotateDegrees;
  bool diagnostics = false;
  int diagnostic_interval_ms = kDefaultDiagnosticIntervalMs;
  uint64_t frame_set_max_skew_ns = kDefaultFrameSetMaxSkewNs;
  uint32_t frame_set_timeout_ms = kDefaultFrameSetTimeoutMs;
  std::string trigger_mode = kDefaultSc132TriggerMode;
  uint32_t imu_sample_rate_hz = kDefaultImuSampleRateHz;
  uint32_t imu_sample_drop_policy = ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED;
  ImuStartOrder imu_start_order = ImuStartOrder::kCameraFirst;
  uint32_t imu_print_rate_hz = kDefaultImuPrintRateHz;
  bool imu_print_metrics = false;
  const FrozenSystemClock* system_clock = nullptr;
  uint32_t record_frame_skip = 0U;
  std::string record_bag_path;
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
  sc132_frame_t* ReleaseOwnership() noexcept;
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
  TimestampDomain group_timestamp_domain = TimestampDomain::kUnknown;
  TimestampDomain camera_timestamp_domain = TimestampDomain::kUnknown;
  TimestampDomain rtsp_timestamp_domain = TimestampDomain::kUnknown;
  uint64_t enqueue_timestamp_ns = 0;
  const void* y_data = nullptr;
  const void* uv_data = nullptr;
  uint64_t y_phys = 0;
  uint64_t uv_phys = 0;
  uint64_t y_size = 0;
  uint64_t uv_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t vstride = 0;
};

struct ImuConsumerOptions {
  uint32_t sample_rate_hz = kDefaultImuSampleRateHz;
  uint32_t sample_drop_policy = ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED;
  uint32_t count = 0U;
  std::atomic<bool>* stop_requested = nullptr;
  const FrozenSystemClock* system_clock = nullptr;
};

#ifdef RELEASE008_TESTING
void ResetImuIdleWaitCountForTest();
uint32_t ImuIdleWaitCountForTest();
std::size_t ImuPendingCapacityForTest();
#endif

using ImuSampleObserver = void (*)(const icm42688_sample_t& sample, void* user);

// adapter 独占 ICM C handle，并在 callback trampoline 截断 observer 异常。
int RunIcmConsumer(const ImuConsumerOptions& options, ImuSampleObserver observer,
                   void* observer_user);

// 临时切换文件描述符为非阻塞模式；生命周期结束时恢复原始 flags。
class ScopedNonblockingFd final {
 public:
  explicit ScopedNonblockingFd(int fd)
      : fd_(fd), original_flags_(fcntl(fd, F_GETFL, 0)) {
    // 仅成功设置非阻塞位后才 armed；失败时调用方必须禁用输出。
    if (original_flags_ >= 0 &&
        fcntl(fd_, F_SETFL, original_flags_ | O_NONBLOCK) == 0) {
      armed_ = true;
    }
  }

  ScopedNonblockingFd(const ScopedNonblockingFd&) = delete;
  ScopedNonblockingFd& operator=(const ScopedNonblockingFd&) = delete;

  ~ScopedNonblockingFd() {
    if (armed_ && fcntl(fd_, F_SETFL, original_flags_) < 0) {
      // 析构路径不得抛异常，但恢复失败必须留下明确诊断。
      constexpr char kWarning[] =
          "warning: failed to restore stdout file status flags\n";
      const ssize_t warning_result =
          ::write(STDERR_FILENO, kWarning, sizeof(kWarning) - 1U);
      static_cast<void>(warning_result);
    }
  }

  bool active() const { return armed_; }

 private:
  int fd_ = -1;
  int original_flags_ = -1;
  bool armed_ = false;
};

struct ImuPrintState {
  uint32_t print_every_samples = 1U;
  uint64_t observed_samples = 0U;
  uint64_t last_timestamp_ns = 0U;
  int output_fd = STDOUT_FILENO;
  bool output_available = true;
  bool print_metrics = false;
  uint64_t dropped_output_lines = 0U;
};

// 按采样率和输出率计算抽样步长；任一输入为 0 时返回 0，表示禁用输出。
uint32_t ImuPrintEverySamples(uint32_t sample_rate_hz, uint32_t print_rate_hz);
// CLI observer 使用单次非阻塞 write；慢/关闭的输出端只丢日志，
// 消费仍覆盖每个 IMU 样本。
void PrintImuSample(const icm42688_sample_t& sample, void* user);

uint64_t SteadyClockNowNs();
int RtspPortForChannel(const Options& options, int channel);
uint32_t CameraMaskFromChannelCount(int channels);
int CameraMaskPopCount(uint32_t camera_mask);
bool CameraMaskContains(uint32_t camera_mask, int camera_id);
bool IsSupportedCameraMask(uint32_t camera_mask);
int OutputWidth(const Options& options);
int OutputHeight(const Options& options);
int InternalRotateDegrees(const Options& options);

}  // namespace robobaton_demo
