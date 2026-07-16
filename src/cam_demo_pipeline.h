#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

extern "C" {
#include "sc132camera.h"
}

#include "cam_demo_common.h"

namespace robobaton_demo {

class RtspChannels;

struct PipelineHooks {
  void (*on_frame_set)(const sc132_frame_set_t& frame_set, void* user) = nullptr;
  void (*on_queued_frame)(const QueuedFrame& frame, void* user) = nullptr;
  void (*before_queue_insert)(void* user) = nullptr;
  // 2026-07-15 修改原因：测试可在 production StartWorkers 路径中注入与
  // std::thread constructor 等价的 create exception，并保留已创建线程 ownership。
  std::thread (*create_thread)(std::function<void()> entry, void* user) = nullptr;
  bool (*join_thread)(std::thread& worker, void* user) = nullptr;
  void* user = nullptr;
};

// 2026-07-15 修改原因：FramePipeline 是 callback context 与 retained-job 的唯一 owner。
// callback 只发布 failure/request_stop；blocking stop 只允许 FinishSc132Shutdown 调用。
class FramePipeline {
 public:
  FramePipeline(Options options, RtspChannels* rtsp, PipelineHooks hooks = {});
  ~FramePipeline();

  FramePipeline(const FramePipeline&) = delete;
  FramePipeline& operator=(const FramePipeline&) = delete;

  void StartWorkers();
  void StartDiagnosticsIfEnabled();
  sc132_frame_set_config_t MakeFrameSetConfig();
  void BeginShutdown(bool request_sc_stop = true) noexcept;
  bool Join() noexcept;

  int32_t FirstError() const noexcept;
  uint64_t TotalSentFrames(int camera_id) const noexcept;
  bool IsQuiescent() const noexcept;
#ifdef RELEASE008_TESTING
  size_t OwnedThreadCountForTesting() const noexcept;
#endif

  // 功能：返回 worker/RTSP 运行阶段是否发生致命错误。
  bool HasFatalError() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  static void FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) noexcept;
};

// 2026-07-15 修改原因：SC 唯一 lifecycle owner 强制 admission-close/request/drain/join，
// 只有 consumer threads 真正 quiescent 后才连续调用 blocking stop 两次。
bool FinishSc132Shutdown(FramePipeline* pipeline) noexcept;

}  // namespace robobaton_demo
