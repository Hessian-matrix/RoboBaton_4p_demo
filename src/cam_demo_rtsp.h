#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "prrtsp_v2.h"

#include "cam_demo_common.h"

namespace robobaton_demo {

using EncodedFrameObserver = void (*)(int camera_id,
                                      const prrtsp_encoded_frame_v2& frame,
                                      void* user);

// opaque handle 槽位固定按物理 camera id 索引。
class RtspChannels {
 public:
  RtspChannels();
  ~RtspChannels() = default;
  RtspChannels(const RtspChannels&) = delete;
  RtspChannels& operator=(const RtspChannels&) = delete;

  bool SetEncodedFrameObserver(EncodedFrameObserver observer, void* user) noexcept;
  int32_t Open(int camera_id, int port, const Options& options) noexcept;
  int32_t Send(int camera_id, QueuedFrame& frame) noexcept;
  bool CaptureStatuses() noexcept;
  bool CloseReverse() noexcept;

  size_t OpenHandleCount() const noexcept;
  const prrtsp_stream_status_v2& Status(int camera_id) const noexcept;
  int32_t LastStatusResult(int camera_id) const noexcept;

 private:
  struct EncodedObserverContext {
    RtspChannels* owner = nullptr;
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
  std::array<uint32_t, kMaxChannels> widths_{};
  std::array<uint32_t, kMaxChannels> heights_{};
  std::array<prrtsp_stream_status_v2, kMaxChannels> statuses_{};
  std::array<int32_t, kMaxChannels> status_results_{};
  std::array<uint32_t, kMaxChannels> close_calls_{};
};

}  // namespace robobaton_demo
