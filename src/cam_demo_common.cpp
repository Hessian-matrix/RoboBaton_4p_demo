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

// 功能：根据图像输出配置计算 RTSP 编码宽度。
// 输入：options.width/options.height/options.rotate_degrees。
// 输出：实际送入编码器的宽度。
int OutputWidth(const Options& options) {
  if (options.rotate_degrees == 90 || options.rotate_degrees == 270) {
    return options.height;
  }
  return options.width;
}

// 功能：根据图像输出配置计算 RTSP 编码高度。
// 输入：options.width/options.height/options.rotate_degrees。
// 输出：实际送入编码器的高度。
int OutputHeight(const Options& options) {
  if (options.rotate_degrees == 90 || options.rotate_degrees == 270) {
    return options.width;
  }
  return options.height;
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
