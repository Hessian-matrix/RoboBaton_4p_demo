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
  void StartRuntimeMonitor();
  void MarkSourceStarted() noexcept;
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

  bool RtspPreviewComplete() const noexcept;
  uint64_t RtspPreviewDroppedFrames(int camera_id) const noexcept;
  int32_t RtspPreviewLastError(int camera_id) const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  static void FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) noexcept;
};

struct Sc132ShutdownResult {
  bool consumer_join_ok = false;
  bool rtsp_status_ok = false;
  bool rtsp_close_ok = false;
  bool sc132_cleanup_reached = false;
  bool ownership_quiescent = false;
};

using Sc132BeforeBlockingStopHook = void (*)(const Sc132ShutdownResult& result,
                                             void* user) noexcept;

Sc132ShutdownResult FinishSc132ShutdownDetailed(
    FramePipeline* pipeline, RtspChannels* rtsp,
    Sc132BeforeBlockingStopHook before_blocking_stop = nullptr,
    void* before_blocking_stop_user = nullptr) noexcept;

// 关闭顺序：admission-close、request、drain、join、RTSP frame release、blocking stop。
bool FinishSc132Shutdown(FramePipeline* pipeline, RtspChannels* rtsp) noexcept;

}  // namespace robobaton_demo
