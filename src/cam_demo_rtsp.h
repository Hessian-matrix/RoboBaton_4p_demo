#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
// 2026-07-16 修改原因：保留 canonical include/pr_venc.h 路径，但其内容已同步为 PRRTSP opaque-handle v2 公共契约。
#include "pr_venc.h"
}

#include "cam_demo_common.h"

namespace robobaton_demo {

// 2026-07-15 修改原因：v2 API 是 opaque handle API；四个槽位固定按物理 camera_id
// 索引，禁止恢复 singleton/ch1..ch4 符号或把初始化顺序当作通道身份。
class RtspChannels {
 public:
  RtspChannels() = default;
  ~RtspChannels() = default;
  RtspChannels(const RtspChannels&) = delete;
  RtspChannels& operator=(const RtspChannels&) = delete;

  int32_t Open(int camera_id, int port, const Options& options) noexcept;
  int32_t Send(int camera_id, const QueuedFrame& frame) noexcept;
  bool CaptureStatuses() noexcept;
  bool CloseReverse() noexcept;

  size_t OpenHandleCount() const noexcept;
  const prrtsp_stream_status_v2& Status(int camera_id) const noexcept;
  int32_t LastStatusResult(int camera_id) const noexcept;

 private:
  static bool ValidPath(const std::string& path) noexcept;
  bool BuildDescriptor(int camera_id, const QueuedFrame& frame,
                       prrtsp_nv12_frame_v2* descriptor) const noexcept;

  std::array<prrtsp_stream_t*, kMaxChannels> handles_{};
  std::array<uint32_t, kMaxChannels> widths_{};
  std::array<uint32_t, kMaxChannels> heights_{};
  std::array<prrtsp_stream_status_v2, kMaxChannels> statuses_{};
  std::array<int32_t, kMaxChannels> status_results_{};
  std::array<uint32_t, kMaxChannels> close_calls_{};
};

}  // namespace robobaton_demo
