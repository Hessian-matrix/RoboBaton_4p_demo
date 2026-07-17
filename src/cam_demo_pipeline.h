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
  // 在真实 StartWorkers 路径注入等价于 thread constructor 的异常。
  std::thread (*create_thread)(std::function<void()> entry, void* user) = nullptr;
  bool (*join_thread)(std::thread& worker, void* user) = nullptr;
  void* user = nullptr;
};

// FramePipeline 唯一持有 callback context 和 retained jobs。
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

// 关闭顺序：admission-close、request、drain、join，quiescent 后 blocking stop。
bool FinishSc132Shutdown(FramePipeline* pipeline) noexcept;

}  // namespace robobaton_demo
