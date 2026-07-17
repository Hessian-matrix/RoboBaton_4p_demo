#include "cam_demo_common.h"

#include <chrono>
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
  enqueue_timestamp_ns = other.enqueue_timestamp_ns;
  y_data = other.y_data;
  uv_data = other.uv_data;
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

uint64_t SteadyClockNowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int RtspPortForChannel(int channel) { return kBaseRtspPort + channel; }

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
  // 2026-07-17 修改原因：width/height 是默认横屏交付画布；外部 0/180 保持画布轴，90/270 才交换。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.height
             : options.width;
}

int OutputHeight(const Options& options) {
  // 2026-07-17 修改原因：与 OutputWidth 共用对外交付坐标系，安装补偿只影响底层角度，不得二次交换画布。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.width
             : options.height;
}

int InternalRotateDegrees(const Options& options) {
  return (options.rotate_degrees + kMountRotateDegrees) % 360;
}

}  // namespace robobaton_demo
