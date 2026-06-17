#include "cam_demo_common.h"

#include <algorithm>
#include <chrono>

namespace robobaton_demo {

std::atomic<bool> g_stop_requested{false};

// 功能：获取进程内单调递增时间，用于队列延迟和诊断统计。
// 输入：无。
// 输出：steady_clock 的纳秒计数，不代表系统墙上时间。
uint64_t SteadyClockNowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// 功能：把相机通道号映射到 libprrtsp 的 ch1/ch2/ch3/ch4 接口。
// 输入：channel 为 0..3。
// 输出：对应 RTSP endpoint；调用方负责保证 channel 合法。
RtspEndpoint RtspEndpointForChannel(int channel) {
  static constexpr std::array<RtspEndpoint, kMaxChannels> kRtspEndpoints = {
      RtspEndpoint::kCh1, RtspEndpoint::kCh2, RtspEndpoint::kCh3, RtspEndpoint::kCh4};
  return kRtspEndpoints[channel];
}

// 功能：计算相机通道对应的 RTSP 服务端口。
// 输入：channel 为 0..3。
// 输出：通道 0/1/2/3 分别对应 554/555/556/557。
int RtspPortForChannel(int channel) {
  return kBaseRtspPort + channel;
}

// 功能：把旧版通道数量转换为物理相机 mask。
// 输入：channels 只保留内部已验证的 1/4 两种路径。
// 输出：channels=1 时启用 cam0，channels=4 时启用四路。
uint32_t CameraMaskFromChannelCount(int channels) {
  if (channels <= 0) {
    return 0;
  }
  if (channels >= kMaxChannels) {
    return kDefaultCameraMask;
  }
  return (1U << static_cast<uint32_t>(channels)) - 1U;
}

// 功能：统计 mask 内有效物理相机数量。
// 输入：只统计 cam0..cam3 四个有效 bit，忽略更高 bit。
// 输出：有效 bit 数量，用于同步帧组 camera_count。
int CameraMaskPopCount(uint32_t camera_mask) {
  int count = 0;
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if ((camera_mask & (1U << static_cast<uint32_t>(camera_id))) != 0) {
      ++count;
    }
  }
  return count;
}

// 功能：判断指定物理相机是否启用。
// 输入：camera_id 越界时直接返回 false。
// 输出：true 表示当前进程会为该物理相机启动 worker/RTSP。
bool CameraMaskContains(uint32_t camera_mask, int camera_id) {
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    return false;
  }
  return (camera_mask & (1U << static_cast<uint32_t>(camera_id))) != 0;
}

// 功能：限制内部诊断入口的 mask 组合。
// 输入：camera_mask bit0..bit3 对应物理 cam0..cam3。
// 输出：只允许单颗 sensor 诊断或完整四目主路径，避免重新引入未验证的 2/3 路组合。
bool IsSupportedCameraMask(uint32_t camera_mask) {
  if (camera_mask == kDefaultCameraMask) {
    return true;
  }
  return CameraMaskPopCount(camera_mask) == 1 &&
         (camera_mask & ~kDefaultCameraMask) == 0;
}

// 功能：根据对外图像配置计算 RTSP 编码宽度。
// 输入：options.width/options.height 是用户看到的正装基准尺寸，rotate_degrees 是对外旋转角度。
// 输出：实际送入编码器的宽度。
int OutputWidth(const Options& options) {
  if (options.rotate_degrees == 90 || options.rotate_degrees == 270) {
    return options.height;
  }
  return options.width;
}

// 功能：根据对外图像配置计算 RTSP 编码高度。
// 输入：options.width/options.height 是用户看到的正装基准尺寸，rotate_degrees 是对外旋转角度。
// 输出：实际送入编码器的高度。
int OutputHeight(const Options& options) {
  if (options.rotate_degrees == 90 || options.rotate_degrees == 270) {
    return options.width;
  }
  return options.height;
}

// 功能：把用户旋转角度映射到底层安装补偿角度。
// 输入：用户看到的正装画面以 rotate=0 表示；SC132 原始安装方向需要内部右旋 90 度。
// 输出：传给 libsc132 的真实旋转角度。
int InternalRotateDegrees(const Options& options) {
  // 2026-06-17 修改原因：对外隐藏 sensor 竖装原始方向；用户 rotate=0 时底层仍执行 90 度安装补偿。
  return (options.rotate_degrees + kMountRotateDegrees) % 360;
}

// 功能：返回 RTSP endpoint 的日志名称。
// 输入：endpoint 枚举。
// 输出："ch1".."ch4"，未知值返回 "unknown"。
const char* RtspEndpointName(RtspEndpoint endpoint) {
  switch (endpoint) {
    case RtspEndpoint::kCh1:
      return "ch1";
    case RtspEndpoint::kCh2:
      return "ch2";
    case RtspEndpoint::kCh3:
      return "ch3";
    case RtspEndpoint::kCh4:
      return "ch4";
  }
  return "unknown";
}

// 功能：释放 QueuedFrame 持有的 libsc132 frame 引用。
// 输入输出：item 非空且包含 frame 时释放引用，并把 item->frame 置空。
// 副作用：对应 DMA buffer 可能在引用计数归零后被底层复用。
void ReleaseQueuedFrame(QueuedFrame* item) {
  if (item != nullptr && item->frame != nullptr) {
    sc132_frame_release(item->frame);
    item->frame = nullptr;
  }
}

}  // namespace robobaton_demo
