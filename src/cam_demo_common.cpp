#include "cam_demo_common.h"

#include <stdexcept>
#include <utility>

extern "C" {
#include "sc132camera.h"
}

namespace robobaton_demo {

std::atomic<bool> g_stop_requested{false};

QueuedFrame::~QueuedFrame() { Reset(); }

QueuedFrame::QueuedFrame(QueuedFrame&& other) noexcept { *this = std::move(other); }

QueuedFrame& QueuedFrame::operator=(QueuedFrame&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  frame = std::exchange(other.frame, nullptr);
  channel = other.channel;
  sequence = other.sequence;
  frame_id = other.frame_id;
  group_id = other.group_id;
  group_timestamp_ns = other.group_timestamp_ns;
  group_max_skew_ns = other.group_max_skew_ns;
  camera_timestamp_ns = other.camera_timestamp_ns;
  rtsp_timestamp_ns = other.rtsp_timestamp_ns;
  group_timestamp_domain = other.group_timestamp_domain;
  camera_timestamp_domain = other.camera_timestamp_domain;
  rtsp_timestamp_domain = other.rtsp_timestamp_domain;
  enqueue_timestamp_ns = other.enqueue_timestamp_ns;
  y_data = other.y_data;
  uv_data = other.uv_data;
  y_phys = other.y_phys;
  uv_phys = other.uv_phys;
  y_size = other.y_size;
  uv_size = other.uv_size;
  width = other.width;
  height = other.height;
  stride = other.stride;
  vstride = other.vstride;
  return *this;
}

void QueuedFrame::Reset() noexcept {
  if (frame != nullptr) {
    // retained frame 由 RAII 唯一管理，所有路径统一经 Reset release。
    sc132_frame_release(frame);
    frame = nullptr;
  }
}

sc132_frame_t* QueuedFrame::ReleaseOwnership() noexcept {
  return std::exchange(frame, nullptr);
}

uint64_t SteadyClockNowNs() {
  uint64_t timestamp_ns = 0U;
  if (FrozenSystemClock::ReadSystemClock(FrozenClockId::kMonotonicRaw,
                                         &timestamp_ns, nullptr) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW read failed");
  }
  return timestamp_ns;
}

int RtspPortForChannel(int channel) { return kBaseRtspPort + channel; }

// 编码格式名称统一用于用户可见的状态和日志输出。
const char* VideoCodecName(VideoCodec codec) noexcept {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264";
    case VideoCodec::kH265:
      return "h265";
  }
  // 非法枚举统一输出 unknown，避免日志路径产生未定义行为。
  return "unknown";
}

const char* ImuSampleDropPolicyName(uint32_t policy) noexcept {
  switch (policy) {
    case ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED:
      return "allow-counted";
    case ICM42688_SAMPLE_DROP_POLICY_STRICT:
      return "strict";
    default:
      return "invalid";
  }
}

const char* TimestampDomainName(TimestampDomain domain) noexcept {
  switch (domain) {
    case TimestampDomain::kMonotonicRaw:
      return "monotonic_raw";
    case TimestampDomain::kSystemRealtime:
      return "system_realtime";
    case TimestampDomain::kSc132Native:
      return "sc132_native";
    case TimestampDomain::kUnknown:
      break;
  }
  return "unknown";
}

bool Sc132TimestampsAreMonotonicRaw(const Options& options) noexcept {
  return options.trigger_mode == "software_gpio" || options.trigger_mode == "gpio";
}

TimestampDomain Sc132OutputTimestampDomain(const Options& options) noexcept {
  if (!Sc132TimestampsAreMonotonicRaw(options)) {
    return TimestampDomain::kSc132Native;
  }
  return options.system_clock != nullptr ? TimestampDomain::kSystemRealtime
                                        : TimestampDomain::kMonotonicRaw;
}

uint32_t CameraMaskFromChannelCount(int channels) {
  if (channels <= 0) {
    return 0U;
  }
  if (channels >= kMaxChannels) {
    return kDefaultCameraMask;
  }
  return (1U << static_cast<uint32_t>(channels)) - 1U;
}

int CameraMaskPopCount(uint32_t camera_mask) {
  int count = 0;
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (CameraMaskContains(camera_mask, camera_id)) {
      ++count;
    }
  }
  return count;
}

bool CameraMaskContains(uint32_t camera_mask, int camera_id) {
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    return false;
  }
  return (camera_mask & (1U << static_cast<uint32_t>(camera_id))) != 0U;
}

bool IsSupportedCameraMask(uint32_t camera_mask) {
  if (camera_mask == kDefaultCameraMask) {
    return true;
  }
  return CameraMaskPopCount(camera_mask) == 1 &&
         (camera_mask & ~kDefaultCameraMask) == 0U;
}

int OutputWidth(const Options& options) {
  // width/height 是默认横屏交付画布；外部 0/180 保持画布轴，90/270 交换宽高。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.height
             : options.width;
}

int OutputHeight(const Options& options) {
  // 安装补偿只影响底层旋转角度，不改变对外交付画布的宽高判定。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.width
             : options.height;
}

int InternalRotateDegrees(const Options& options) {
  return (options.rotate_degrees + kMountRotateDegrees) % 360;
}

}  // namespace robobaton_demo
