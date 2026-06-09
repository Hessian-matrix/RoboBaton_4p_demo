#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "sc132camera.h"
}

namespace robobaton_demo {

// Demo 支持的最大相机路数。
constexpr int kMaxChannels = 4;
constexpr int kDefaultWidth = 1088;
constexpr int kDefaultHeight = 1280;
constexpr int kDefaultFps = 60;
// 默认单路编码码率，单位为 kbps。
constexpr long long kDefaultBps = 2000;
// 默认图像输出配置。
constexpr int kDefaultRotateDegrees = 90;
constexpr int kBaseRtspPort = 554;
constexpr size_t kQueueCapacity = 10;
constexpr int kDefaultDiagnosticIntervalMs = 1000;
constexpr uint64_t kDefaultFrameSetMaxSkewNs = 1000000ULL;
constexpr uint32_t kDefaultFrameSetTimeoutMs = 100;
constexpr const char* kDefaultSc132TriggerMode = "software_gpio";

extern std::atomic<bool> g_stop_requested;

// RTSP 编码库暴露的四路通道入口。
enum class RtspEndpoint {
  kCh1,
  kCh2,
  kCh3,
  kCh4,
};

// Demo 运行参数。
// 输入来源：命令行参数或默认值。
// 输出用途：相机初始化、RTSP 初始化、同步诊断和触发模式配置。
struct Options {
  int channels = kMaxChannels;
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

// 已入队的相机帧元数据。
// 生命周期：frame 已 retain；消费线程处理完成后必须调用 ReleaseQueuedFrame。
// 时间戳：camera_timestamp_ns 来自相机帧，rtsp_timestamp_ns 用作推流 PTS。
// 图像数据：y/uv 虚拟地址和物理地址指向 NV12 DMA buffer，不能长期保存裸指针。
struct QueuedFrame {
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
  void* y_data = nullptr;
  void* uv_data = nullptr;
  uint64_t y_phys = 0;
  uint64_t uv_phys = 0;
  long y_len = 0;
  long uv_len = 0;
  int width = 0;
  int height = 0;
};

// 功能：读取 steady_clock 当前时间。
// 输入：无。
// 输出：单调时钟纳秒时间戳。
uint64_t SteadyClockNowNs();

// 功能：把相机通道号映射到 RTSP 库通道枚举。
// 输入：channel 为 0..3。
// 输出：对应的 RTSP endpoint。
RtspEndpoint RtspEndpointForChannel(int channel);

// 功能：返回指定相机通道使用的 RTSP 端口。
// 输入：channel 为 0..3。
// 输出：端口号，通道 0 对应 554，之后依次递增。
int RtspPortForChannel(int channel);

// 功能：计算实际输出宽度。
// 输入：运行参数 options。
// 输出：RTSP 编码侧实际宽度。
int OutputWidth(const Options& options);

// 功能：计算实际输出高度。
// 输入：运行参数 options。
// 输出：RTSP 编码侧实际高度。
int OutputHeight(const Options& options);

// 功能：返回 RTSP endpoint 的可读名称。
// 输入：RTSP endpoint 枚举。
// 输出：用于日志显示的字符串。
const char* RtspEndpointName(RtspEndpoint endpoint);

// 功能：释放 QueuedFrame 持有的相机帧引用。
// 输入输出：item 可为空；非空时会清空 item->frame。
// 副作用：调用 libsc132 的 sc132_frame_release，归还 DMA frame 引用。
void ReleaseQueuedFrame(QueuedFrame* item);

}  // namespace robobaton_demo
