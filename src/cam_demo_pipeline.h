#pragma once

#include <memory>

#include "cam_demo_common.h"

namespace robobaton_demo {

// 功能：单帧入队后的用户扩展入口。
// 输入：frame 已 retain，包含 NV12 地址、时间戳、序号和同步组信息。
// 输出：无。
// 生命周期：函数返回后 demo 会继续推流并释放 frame；异步保存图像时需自行复制或 retain。
void OnQueuedCameraFrame(const QueuedFrame& frame);

// 功能：四路帧组通过 libsc132 同步匹配后的用户扩展入口。
// 输入：frame_set 包含同一 group_id 的多路相机帧和组时间戳。
// 输出：无。
// 生命周期：如需在函数外继续使用 frame_set 内的 frame，用户需自行 retain/release。
void OnSynchronizedFrameSet(const sc132_frame_set_t& frame_set);

// 功能：管理相机帧入队、四路对齐发送、RTSP 推流和诊断线程。
// 输入：构造时传入 Options。
// 输出：通过 MakeFrameSetConfig 提供 libsc132 回调配置。
// 副作用：StartWorkers 后创建后台线程；Stop/Join 负责退出和回收。
class FramePipeline {
 public:
  // 功能：创建帧处理流水线。
  // 输入：options 为运行参数副本。
  explicit FramePipeline(Options options);

  // 功能：析构时停止并回收后台线程。
  ~FramePipeline();

  FramePipeline(const FramePipeline&) = delete;
  FramePipeline& operator=(const FramePipeline&) = delete;

  // 功能：启动每路 RTSP 发送 worker。
  void StartWorkers();

  // 功能：按配置启动诊断线程；未开启 diagnostics 时不创建线程。
  void StartDiagnosticsIfEnabled();

  // 功能：生成 libsc132 帧组回调配置。
  // 输出：包含回调函数、用户指针、通道数和帧组超时参数。
  sc132_frame_set_config_t MakeFrameSetConfig();

  // 功能：通知所有队列、barrier 和线程退出。
  void Stop();

  // 功能：等待后台线程全部退出。
  void Join();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  static void FrameSetCallback(const sc132_frame_set_t* frame_set, void* user);
};

}  // namespace robobaton_demo
