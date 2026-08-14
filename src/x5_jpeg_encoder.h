#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#include <vector>

#include "cam_demo_common.h"

namespace robobaton_demo {

struct X5JpegNv12CopyResult {
  uint64_t bulk_plane_count = 0U;
  uint64_t bulk_bytes = 0U;
  uint64_t row_copy_count = 0U;
  uint64_t row_bytes = 0U;
};


struct X5JpegInputSlot {
  int camera_id = 0;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
  uint8_t* y_data = nullptr;
  uint8_t* uv_data = nullptr;
  uint64_t y_phys = 0U;
  uint64_t uv_phys = 0U;
  size_t y_capacity = 0U;
  size_t uv_capacity = 0U;
  int y_fd = -1;
  int uv_fd = -1;
  bool submitted_to_hardware = false;
};

struct X5JpegEncodeRequest {
  int camera_id = 0;
  uint64_t pts_us = 0U;
  const X5JpegInputSlot* slot = nullptr;
};

// 私有硬件 JPEG 适配器。每路相机固定一个 encoder context，输入只接受 recorder 自有 NV12 图形内存。
class X5JpegEncoder final {
 public:
  static constexpr size_t kCameraCount = kMaxChannels;
  static constexpr size_t kSlotsPerCamera = 4U;
  static constexpr uint32_t kQuality = 80U;

  X5JpegEncoder();
  ~X5JpegEncoder();

  X5JpegEncoder(const X5JpegEncoder&) = delete;
  X5JpegEncoder& operator=(const X5JpegEncoder&) = delete;

  void Start(uint32_t camera_mask, uint32_t width, uint32_t height);
  bool Stop(std::string* first_error = nullptr) noexcept;
  bool started() const noexcept { return started_; }

  X5JpegInputSlot* AcquireSlot(int camera_id) noexcept;
  X5JpegInputSlot* WaitAcquireSlot(int camera_id,
                                   const std::atomic<bool>& stop_requested) noexcept;
  void NotifySlotWaiters() noexcept;
  void ReleaseSlot(X5JpegInputSlot* slot) noexcept;
  void QuarantineSlot(X5JpegInputSlot* slot) noexcept;
  X5JpegNv12CopyResult CopyNv12ToSlot(const QueuedFrame& frame, X5JpegInputSlot* slot);
  void Encode(const X5JpegEncodeRequest& request, std::vector<uint8_t>* jpeg);
  // 将 JPEG 直接追加到已有 ROS payload，返回本次追加的 JPEG 有效字节数。
  void EncodeAppend(const X5JpegEncodeRequest& request, std::vector<uint8_t>* payload,
                    size_t* jpeg_size);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool started_ = false;
};

}  // namespace robobaton_demo
