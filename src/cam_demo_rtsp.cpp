#include "cam_demo_rtsp.h"

#include <cstring>
#include <limits>

namespace robobaton_demo {
namespace {

constexpr uint32_t kCloseAttemptLimit = 3U;

bool ValidCameraId(int camera_id) {
  return camera_id >= 0 && camera_id < kMaxChannels;
}

}  // namespace

bool RtspChannels::ValidPath(const std::string& path) noexcept {
  if (path.size() < 2U || path.size() > PRRTSP_PATH_CONTENT_MAX_BYTES_V2_0 ||
      path.front() != '/') {
    return false;
  }
  for (unsigned char character : path) {
    // v2 path 拒绝控制字符、空白及 URI 歧义字符。
    if (character < 0x21U || character > 0x7eU || character == '?' ||
        character == '#' || character == '%' || character == '\\') {
      return false;
    }
  }
  return true;
}

int32_t RtspChannels::Open(int camera_id, int port, const Options& options) noexcept {
  if (!ValidCameraId(camera_id) || handles_[camera_id] != nullptr ||
      port <= 0 || options.fps <= 0 || options.bps <= 0 ||
      static_cast<unsigned long long>(options.bps) >
          static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max()) ||
      !ValidPath(options.url)) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }

  const int output_width = OutputWidth(options);
  const int output_height = OutputHeight(options);
  if (output_width <= 0 || output_height <= 0 || (output_width & 1) != 0 ||
      (output_height & 1) != 0) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }

  // 在 consumer 边界将强类型编码格式映射到 PRRTSP v2.1 ABI。
  uint32_t codec = PRRTSP_CODEC_DEFAULT;
  switch (options.video_codec) {
    case VideoCodec::kH264:
      codec = PRRTSP_CODEC_H264;
      break;
    case VideoCodec::kH265:
      codec = PRRTSP_CODEC_H265;
      break;
    default:
      return PRRTSP_E_UNSUPPORTED;
  }

  prrtsp_stream_config_v2 config{};
  config.struct_size = PRRTSP_STREAM_CONFIG_V2_1_SIZE;
  config.flags = 0U;
  config.width = static_cast<uint32_t>(output_width);
  config.height = static_cast<uint32_t>(output_height);
  config.fps_num = static_cast<uint32_t>(options.fps);
  config.fps_den = 1U;
  config.bitrate_kbps = static_cast<uint32_t>(options.bps);
  config.rotation_clockwise = 0U;
  config.port = static_cast<uint32_t>(port);
  config.operation_timeout_ms = 1000U;
  config.codec = codec;
  std::memcpy(config.path, options.url.data(), options.url.size());

  prrtsp_stream_t* opened = nullptr;
  const int32_t result = prrtsp_stream_open(&config, &opened);
  // 失败 open 仍可能返回 handle，必须先接管再返回状态。
  if (opened != nullptr) {
    handles_[camera_id] = opened;
    widths_[camera_id] = config.width;
    heights_[camera_id] = config.height;
  }
  return result;
}

bool RtspChannels::BuildDescriptor(int camera_id, const QueuedFrame& frame,
                                   prrtsp_nv12_frame_v2* descriptor) const noexcept {
  if (!ValidCameraId(camera_id) || descriptor == nullptr ||
      handles_[camera_id] == nullptr || frame.frame == nullptr ||
      frame.y_data == nullptr || frame.uv_data == nullptr || frame.width == 0U ||
      frame.height == 0U || (frame.width & 1U) != 0U || (frame.height & 1U) != 0U ||
      frame.width != widths_[camera_id] || frame.height != heights_[camera_id] ||
      frame.stride != frame.width || frame.vstride != frame.height) {
    return false;
  }

  if (frame.width > std::numeric_limits<uint64_t>::max() / frame.height) {
    return false;
  }
  const uint64_t y_required = static_cast<uint64_t>(frame.width) * frame.height;
  const uint32_t uv_height = frame.height / 2U;
  if (frame.width > std::numeric_limits<uint64_t>::max() / uv_height) {
    return false;
  }
  const uint64_t uv_required = static_cast<uint64_t>(frame.width) * uv_height;
  const uintptr_t y_virtual = reinterpret_cast<uintptr_t>(frame.y_data);
  const uintptr_t uv_virtual = reinterpret_cast<uintptr_t>(frame.uv_data);
  if (y_virtual == 0U || uv_virtual == 0U || frame.y_size < y_required ||
      frame.uv_size < uv_required) {
    return false;
  }

  prrtsp_nv12_frame_v2 value{};
  value.struct_size = PRRTSP_NV12_FRAME_V2_0_SIZE;
  value.flags = 0U;
  value.width = frame.width;
  value.height = frame.height;
  value.y_stride = frame.width;
  value.uv_stride = frame.width;
  value.y_vstride = frame.height;
  value.uv_vstride = uv_height;
  value.y_virtual_address = static_cast<uint64_t>(y_virtual);
  value.uv_virtual_address = static_cast<uint64_t>(uv_virtual);
  value.y_physical_address = 0U;
  value.uv_physical_address = 0U;
  value.y_size_bytes = frame.y_size;
  value.uv_size_bytes = frame.uv_size;
  value.timestamp_ns = frame.rtsp_timestamp_ns;
  *descriptor = value;
  return true;
}

int32_t RtspChannels::Send(int camera_id, const QueuedFrame& frame) noexcept {
  prrtsp_nv12_frame_v2 descriptor{};
  if (!BuildDescriptor(camera_id, frame, &descriptor)) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }
  return prrtsp_stream_send(handles_[camera_id], &descriptor);
}

bool RtspChannels::CaptureStatuses() noexcept {
  bool success = true;
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (handles_[camera_id] == nullptr) {
      continue;
    }
    prrtsp_stream_status_v2 status{};
    status.struct_size = PRRTSP_STREAM_STATUS_V2_0_SIZE;
    const int32_t result = prrtsp_stream_get_status(handles_[camera_id], &status);
    statuses_[camera_id] = status;
    status_results_[camera_id] = result;
    if (result != PRRTSP_OK) {
      success = false;
    }
  }
  return success;
}

bool RtspChannels::CloseReverse() noexcept {
  bool success = true;
  for (int camera_id = kMaxChannels - 1; camera_id >= 0; --camera_id) {
    while (handles_[camera_id] != nullptr &&
           close_calls_[camera_id] < kCloseAttemptLimit) {
      ++close_calls_[camera_id];
      (void)prrtsp_stream_close(&handles_[camera_id]);
    }
    // close 最多三次；handle 仍非空表示 cleanup 未完成。
    if (handles_[camera_id] != nullptr) {
      success = false;
    }
  }
  return success;
}

size_t RtspChannels::OpenHandleCount() const noexcept {
  size_t count = 0U;
  for (const prrtsp_stream_t* handle : handles_) {
    if (handle != nullptr) {
      ++count;
    }
  }
  return count;
}

const prrtsp_stream_status_v2& RtspChannels::Status(int camera_id) const noexcept {
  return statuses_[camera_id];
}

int32_t RtspChannels::LastStatusResult(int camera_id) const noexcept {
  return status_results_[camera_id];
}

}  // namespace robobaton_demo
