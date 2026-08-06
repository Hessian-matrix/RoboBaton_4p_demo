#include "mosaic_nv12.h"

#include <cstring>
#include <limits>

namespace robobaton_demo {
namespace {

bool PlaneSizeIsValid(uint32_t stride, uint32_t rows, uint64_t size_bytes) noexcept {
  if (rows == 0U) {
    return false;
  }
  if (stride > std::numeric_limits<uint64_t>::max() / rows) {
    return false;
  }
  return size_bytes >= static_cast<uint64_t>(stride) * rows;
}

bool SourceIsValid(const Nv12ImageView& source) noexcept {
  if (source.y_data == nullptr || source.uv_data == nullptr) {
    return false;
  }
  if (source.width != kMosaicInputWidth || source.height != kMosaicInputHeight) {
    return false;
  }
  if (source.stride < source.width || source.vstride < source.height ||
      (source.vstride & 1U) != 0U) {
    return false;
  }
  return PlaneSizeIsValid(source.stride, source.vstride, source.y_size_bytes) &&
         PlaneSizeIsValid(source.stride, source.vstride / 2U, source.uv_size_bytes);
}

bool DestinationIsValid(const MutableNv12ImageView& destination) noexcept {
  if (destination.y_data == nullptr || destination.uv_data == nullptr) {
    return false;
  }
  if (destination.width != kMosaicOutputWidth || destination.height != kMosaicOutputHeight) {
    return false;
  }
  if (destination.stride < destination.width || destination.vstride < destination.height ||
      (destination.vstride & 1U) != 0U) {
    return false;
  }
  return PlaneSizeIsValid(destination.stride, destination.vstride,
                          destination.y_size_bytes) &&
         PlaneSizeIsValid(destination.stride, destination.vstride / 2U,
                          destination.uv_size_bytes);
}

void CopyPlaneTile(const uint8_t* source, uint32_t source_stride, uint8_t* destination,
                   uint32_t destination_stride, uint32_t tile_x, uint32_t tile_y,
                   uint32_t copy_width, uint32_t copy_height) noexcept {
  for (uint32_t row = 0U; row < copy_height; ++row) {
    std::memcpy(destination + static_cast<uint64_t>(tile_y + row) * destination_stride + tile_x,
                source + static_cast<uint64_t>(row) * source_stride, copy_width);
  }
}

}  // namespace

const char* MosaicNv12StatusName(MosaicNv12Status status) noexcept {
  switch (status) {
    case MosaicNv12Status::kOk:
      return "ok";
    case MosaicNv12Status::kInvalidArgument:
      return "invalid_argument";
    case MosaicNv12Status::kInvalidSource:
      return "invalid_source";
    case MosaicNv12Status::kInvalidDestination:
      return "invalid_destination";
  }
  return "unknown";
}

MosaicNv12Status CopyNv12Mosaic2x2(
    const std::array<Nv12ImageView, kMosaicCameraCount>& sources,
    const MutableNv12ImageView& destination) noexcept {
  if (!DestinationIsValid(destination)) {
    return MosaicNv12Status::kInvalidDestination;
  }
  for (const Nv12ImageView& source : sources) {
    if (!SourceIsValid(source)) {
      return MosaicNv12Status::kInvalidSource;
    }
  }

  for (uint32_t camera = 0U; camera < kMosaicCameraCount; ++camera) {
    const Nv12ImageView& source = sources[camera];
    const uint32_t tile_x = (camera % 2U) * kMosaicInputWidth;
    const uint32_t tile_y = (camera / 2U) * kMosaicInputHeight;
    // Y 平面按原始行高复制；source stride padding 不进入输出画布。
    CopyPlaneTile(source.y_data, source.stride, destination.y_data, destination.stride,
                  tile_x, tile_y, kMosaicInputWidth, kMosaicInputHeight);
    // UV 平面是 NV12 交错 chroma，垂直方向高度为 Y 的一半。
    CopyPlaneTile(source.uv_data, source.stride, destination.uv_data, destination.stride,
                  tile_x, tile_y / 2U, kMosaicInputWidth, kMosaicInputHeight / 2U);
  }
  return MosaicNv12Status::kOk;
}

}  // namespace robobaton_demo
