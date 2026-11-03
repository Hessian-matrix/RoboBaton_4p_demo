#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "prrtsp_v2.h"

#include "cam_demo_common.h"

namespace robobaton_demo {

using EncodedFrameObserver = void (*)(int camera_id,
                                      const prrtsp_encoded_frame_v2& frame,
                                      void* user);

bool EncodingRequired(const Options& options) noexcept;

// opaque handle 槽位固定按物理 camera id 索引。
class MediaChannels {
 public:
  MediaChannels();
  ~MediaChannels() = default;
  MediaChannels(const MediaChannels&) = delete;
  MediaChannels& operator=(const MediaChannels&) = delete;

  bool SetEncodedFrameObserver(EncodedFrameObserver observer, void* user) noexcept;
  // 第二个 observer slot：供 tee 采集与 MP4 消费者同时订阅编码 access unit。
  bool SetCaptureEncodedFrameObserver(EncodedFrameObserver observer, void* user) noexcept;
  bool SetWebEncodedFrameObserver(EncodedFrameObserver observer, void* user) noexcept;
  // 在 send 已返回后关闭失败 sink；不重送已消费的源帧租约。
  int32_t DisableRtsp(int camera_id, const Options& options) noexcept;
  int32_t Open(int camera_id, int port, const Options& options) noexcept;
  int32_t Send(int camera_id, QueuedFrame& frame) noexcept;
  bool CaptureStatuses() noexcept;
  bool CloseReverse() noexcept;

  size_t OpenHandleCount() const noexcept;
  const prrtsp_stream_status_v2& Status(int camera_id) const noexcept;
  int32_t LastStatusResult(int camera_id) const noexcept;
  // Producer status counters belong to this encoder generation, not the application run.
  uint64_t StreamGeneration(int camera_id) const noexcept;

 private:
  struct EncodedObserverContext {
    MediaChannels* owner = nullptr;
    int camera_id = -1;
  };

  static void EncodedFrameBridge(const prrtsp_encoded_frame_v2* frame,
                                 void* user) noexcept;
  static bool ValidPath(const std::string& path) noexcept;
  bool BuildDescriptor(int camera_id, const QueuedFrame& frame,
                       prrtsp_nv12_frame_v2* descriptor) const noexcept;

  std::array<prrtsp_stream_t*, kMaxChannels> handles_{};
  std::array<EncodedObserverContext, kMaxChannels> encoded_contexts_{};
  EncodedFrameObserver encoded_observer_ = nullptr;
  void* encoded_observer_user_ = nullptr;
  EncodedFrameObserver capture_encoded_observer_ = nullptr;
  void* capture_encoded_observer_user_ = nullptr;
  EncodedFrameObserver web_encoded_observer_ = nullptr;
  void* web_encoded_observer_user_ = nullptr;
  std::array<uint32_t, kMaxChannels> widths_{};
  std::array<uint32_t, kMaxChannels> heights_{};
  std::array<prrtsp_stream_status_v2, kMaxChannels> statuses_{};
  std::array<int32_t, kMaxChannels> status_results_{};
  std::array<uint32_t, kMaxChannels> close_calls_{};
  std::array<bool, kMaxChannels> rtsp_disabled_{};
  std::array<std::atomic<uint64_t>, kMaxChannels> stream_generations_{};
};

}  // namespace robobaton_demo
