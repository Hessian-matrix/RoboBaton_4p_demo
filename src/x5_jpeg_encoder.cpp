#include "x5_jpeg_encoder.h"

#include <algorithm>
#include <condition_variable>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <hb_media_codec.h>
#include <hb_mem_mgr.h>

namespace robobaton_demo {
namespace {

constexpr int kCodecTimeoutMs = 2000;
constexpr size_t kMaxJpegOutputBuffers = 64U;

uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept {
  return ((value + alignment - 1U) / alignment) * alignment;
}

std::string StageError(int camera_id, const char* stage, int result) {
  std::ostringstream stream;
  stream << "hardware JPEG camera=" << camera_id << " stage=" << stage
         << " result=" << result;
  return stream.str();
}

bool TryResolveJpegPayloadLength(const uint8_t* data, size_t driver_size,
                                 size_t capacity, size_t* payload) {
  if (driver_size == 0U) {
    throw std::runtime_error("hardware JPEG malformed output: missing payload size");
  }
  if (driver_size > capacity) {
    throw std::runtime_error("hardware JPEG malformed output: payload size exceeds bounds");
  }
  if (data == nullptr || driver_size < 4U || data[0] != 0xFFU || data[1] != 0xD8U) {
    throw std::runtime_error("hardware JPEG malformed output: missing SOI");
  }
  // 部分 X5 Media Codec 固件把 vstream_buf.size 填成输出缓冲容量；真实 JPEG
  // 负载以最后一个 EOI 标记结束，避免把尾部填充写入 bag。
  for (size_t candidate = driver_size; candidate >= 4U; --candidate) {
    if (data[candidate - 2U] == 0xFFU && data[candidate - 1U] == 0xD9U) {
      *payload = candidate;
      return true;
    }
  }
  return false;
}


bool IsLastJpegOutputSlice(const media_codec_output_buffer_info_t& info) noexcept {
  const hb_u32 slice_num = info.jpeg_stream_info.slice_num;
  // slice_num 为 0 表示固件未报告分片总数，不能把首个无 EOI 输出判定为终止。
  return slice_num != 0U && info.jpeg_stream_info.slice_idx >= slice_num - 1U;
}

uint32_t CheckedJpegBitstreamCapacity(uint32_t width, uint32_t height) {
  const uint64_t pixels = static_cast<uint64_t>(width) * height;
  const uint64_t nv12_capacity = pixels + pixels / 2U;
  const uint64_t aligned = ((nv12_capacity + 4095U) / 4096U) * 4096U;
  if (nv12_capacity > std::numeric_limits<uint32_t>::max() ||
      aligned > std::numeric_limits<uint32_t>::max()) {
    throw std::overflow_error("hardware JPEG bitstream capacity overflow");
  }
  return static_cast<uint32_t>(aligned);
}

// 行内没有 padding 时直接拷贝整块有效字节；否则逐行拷贝以兼容任意 stride。
void CopyNv12Plane(const uint8_t* src, uint8_t* dst, uint32_t width,
                   uint32_t rows, uint32_t src_stride, uint32_t dst_stride,
                   bool contiguous_rows, X5JpegNv12CopyResult* result) {
  const uint64_t copy_bytes = static_cast<uint64_t>(width) * rows;
  if (contiguous_rows) {
    std::memcpy(dst, src, static_cast<size_t>(copy_bytes));
    ++result->bulk_plane_count;
    result->bulk_bytes += copy_bytes;
    return;
  }
  for (uint32_t row = 0U; row < rows; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * dst_stride,
                src + static_cast<size_t>(row) * src_stride, width);
  }
  result->row_copy_count += rows;
  result->row_bytes += copy_bytes;
}

}  // namespace

struct X5JpegEncoder::Impl {
  struct Slot {
    X5JpegInputSlot public_slot;
    hb_mem_graphic_buf_t graph{};
    bool allocated = false;
    bool in_use = false;
    bool quarantined = false;
  };

  struct Camera {
    media_codec_context_t context{};
    bool initialized = false;
    bool started = false;
    std::array<Slot, kSlotsPerCamera> slots{};
  };

  std::array<Camera, kCameraCount> cameras{};
  std::mutex slot_mutex;
  std::condition_variable slot_condition;
  bool memory_open = false;
  uint32_t camera_mask = 0U;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t stride = 0U;
  uint32_t vstride = 0U;
  uint32_t bitstream_capacity = 0U;

  void RememberError(std::string* first_error, const std::string& error) noexcept {
    if (first_error != nullptr && first_error->empty()) {
      *first_error = error;
    }
  }

  bool Cleanup(std::string* first_error) noexcept {
    for (Camera& camera : cameras) {
      if (camera.started) {
        const int result = hb_mm_mc_stop(&camera.context);
        if (result == 0) {
          camera.started = false;
        } else {
          RememberError(first_error, StageError(camera.context.instance_index, "stop", result));
        }
      }
      if (camera.initialized) {
        const int result = hb_mm_mc_release(&camera.context);
        if (result == 0) {
          camera.initialized = false;
          camera.started = false;
        } else {
          RememberError(first_error, StageError(camera.context.instance_index, "release", result));
        }
      }
      const bool context_released = !camera.started && !camera.initialized;
      if (context_released) {
        for (Slot& slot : camera.slots) {
          if (slot.allocated) {
            const int fd = slot.graph.fd[0];
            if (fd >= 0) {
              const int result = hb_mem_free_buf(fd);
              if (result != 0) {
                RememberError(first_error, StageError(slot.public_slot.camera_id,
                                                      "free_graph_buf", result));
                continue;
              }
            }
            slot = Slot{};
            slot.public_slot.y_fd = -1;
            slot.public_slot.uv_fd = -1;
          }
        }
      }
    }
    if (memory_open) {
      const bool resources_released =
          std::all_of(cameras.begin(), cameras.end(), [](const Camera& camera) {
            const bool contexts_released = !camera.started && !camera.initialized;
            const bool slots_released = std::all_of(
                camera.slots.begin(), camera.slots.end(),
                [](const Slot& slot) { return !slot.allocated; });
            return contexts_released && slots_released;
          });
      if (resources_released) {
        const int result = hb_mem_module_close();
        if (result == 0) {
          memory_open = false;
        } else {
          RememberError(first_error, StageError(-1, "mem_module_close", result));
        }
      }
    }
    return first_error == nullptr || first_error->empty();
  }

  void AllocateSlot(int camera_id, Slot* slot) {
    if (slot == nullptr) {
      throw std::runtime_error("hardware JPEG missing staging slot");
    }
    constexpr int64_t kFlags = HB_MEM_USAGE_CPU_READ_OFTEN |
                               HB_MEM_USAGE_CPU_WRITE_OFTEN |
                               HB_MEM_USAGE_CACHED |
                               HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF |
                               HB_MEM_USAGE_HW_JPEG_CODEC;
    const int result = hb_mem_alloc_graph_buf(static_cast<int32_t>(width),
                                              static_cast<int32_t>(height),
                                              MEM_PIX_FMT_NV12, kFlags,
                                              static_cast<int32_t>(stride),
                                              static_cast<int32_t>(vstride),
                                              &slot->graph);
    if (result != 0) {
      throw std::runtime_error(StageError(camera_id, "alloc_graph_buf", result));
    }
    slot->allocated = true;
    slot->public_slot.camera_id = camera_id;
    slot->public_slot.width = width;
    slot->public_slot.height = height;
    slot->public_slot.stride = stride;
    slot->public_slot.vstride = vstride;
    slot->public_slot.y_data = slot->graph.virt_addr[0];
    slot->public_slot.uv_data = slot->graph.virt_addr[1];
    slot->public_slot.y_phys = slot->graph.phys_addr[0];
    slot->public_slot.uv_phys = slot->graph.phys_addr[1];
    slot->public_slot.y_capacity = slot->graph.size[0];
    slot->public_slot.uv_capacity = slot->graph.size[1];
    slot->public_slot.y_fd = slot->graph.fd[0];
    slot->public_slot.uv_fd = slot->graph.fd[1];
    slot->public_slot.submitted_to_hardware = false;
    if (slot->public_slot.y_data == nullptr || slot->public_slot.uv_data == nullptr ||
        slot->public_slot.y_phys == 0U || slot->public_slot.uv_phys == 0U ||
        slot->public_slot.y_capacity < static_cast<size_t>(stride) * vstride ||
        slot->public_slot.uv_capacity < static_cast<size_t>(stride) * (vstride / 2U)) {
      throw std::runtime_error(StageError(camera_id, "alloc_graph_buf_validate", -1));
    }
  }

  void StartCamera(int camera_id) {
    Camera& camera = cameras[static_cast<size_t>(camera_id)];
    int result = hb_mm_mc_get_default_context(MEDIA_CODEC_ID_JPEG, true, &camera.context);
    if (result != 0) {
      throw std::runtime_error(StageError(camera_id, "get_default_context", result));
    }
    camera.context.codec_id = MEDIA_CODEC_ID_JPEG;
    camera.context.encoder = true;
    camera.context.video_enc_params.width = static_cast<int32_t>(width);
    camera.context.video_enc_params.height = static_cast<int32_t>(height);
    camera.context.video_enc_params.pix_fmt = MC_PIXEL_FORMAT_NV12;
    camera.context.video_enc_params.frame_buf_count = 1U;
    camera.context.video_enc_params.external_frame_buf = true;
    camera.context.video_enc_params.bitstream_buf_count = 1U;
    camera.context.video_enc_params.bitstream_buf_size = bitstream_capacity;
    camera.context.video_enc_params.jpeg_enc_config.quality_factor = kQuality;
    camera.context.video_enc_params.jpeg_enc_config.restart_interval = width / 16U;
    result = hb_mm_mc_initialize(&camera.context);
    if (result != 0) {
      throw std::runtime_error(StageError(camera_id, "initialize", result));
    }
    camera.initialized = true;
    result = hb_mm_mc_configure(&camera.context);
    if (result != 0) {
      throw std::runtime_error(StageError(camera_id, "configure", result));
    }
    mc_av_codec_startup_params_t startup{};
    startup.video_enc_startup_params.receive_frame_number = 0;
    result = hb_mm_mc_start(&camera.context, &startup);
    if (result != 0) {
      throw std::runtime_error(StageError(camera_id, "start", result));
    }
    camera.started = true;
  }
};

X5JpegEncoder::X5JpegEncoder() : impl_(new Impl()) {}

X5JpegEncoder::~X5JpegEncoder() {
  Stop();
  delete impl_;
}

void X5JpegEncoder::Start(uint32_t camera_mask, uint32_t width, uint32_t height) {
  if (started_) {
    throw std::logic_error("hardware JPEG encoder already started");
  }
  if (width == 0U || height == 0U || (width % 2U) != 0U || (height % 2U) != 0U) {
    throw std::invalid_argument("invalid hardware JPEG dimensions");
  }
  impl_->camera_mask = camera_mask;
  impl_->width = width;
  impl_->height = height;
  impl_->stride = AlignUp(width, 16U);
  impl_->vstride = AlignUp(height, 16U);
  impl_->bitstream_capacity = CheckedJpegBitstreamCapacity(impl_->stride, impl_->vstride);
  const int open_result = hb_mem_module_open();
  if (open_result != 0) {
    throw std::runtime_error(StageError(-1, "mem_module_open", open_result));
  }
  impl_->memory_open = true;
  try {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(camera_mask, camera_id)) {
        continue;
      }
      for (Impl::Slot& slot : impl_->cameras[static_cast<size_t>(camera_id)].slots) {
        impl_->AllocateSlot(camera_id, &slot);
      }
      impl_->StartCamera(camera_id);
    }
  } catch (...) {
    std::string cleanup_error;
    (void)impl_->Cleanup(&cleanup_error);
    throw;
  }
  started_ = true;
}

bool X5JpegEncoder::Stop(std::string* first_error) noexcept {
  if (impl_ != nullptr) {
    const bool ok = impl_->Cleanup(first_error);
    if (ok) {
      started_ = false;
    }
    impl_->slot_condition.notify_all();
    return ok;
  }
  started_ = false;
  return true;
}

X5JpegInputSlot* X5JpegEncoder::AcquireSlot(int camera_id) noexcept {
  if (!started_ || camera_id < 0 || camera_id >= kMaxChannels ||
      !CameraMaskContains(impl_->camera_mask, camera_id)) {
    return nullptr;
  }
  Impl::Camera& camera = impl_->cameras[static_cast<size_t>(camera_id)];
  std::lock_guard<std::mutex> lock(impl_->slot_mutex);
  for (Impl::Slot& slot : camera.slots) {
    if (slot.allocated && !slot.in_use && !slot.quarantined) {
      slot.in_use = true;
      slot.public_slot.submitted_to_hardware = false;
      return &slot.public_slot;
    }
  }
  return nullptr;
}

X5JpegInputSlot* X5JpegEncoder::WaitAcquireSlot(
    int camera_id, const std::atomic<bool>& stop_requested) noexcept {
  if (!started_ || camera_id < 0 || camera_id >= kMaxChannels ||
      !CameraMaskContains(impl_->camera_mask, camera_id)) {
    return nullptr;
  }
  Impl::Camera& camera = impl_->cameras[static_cast<size_t>(camera_id)];
  std::unique_lock<std::mutex> lock(impl_->slot_mutex);
  const auto find_available_slot = [&]() -> Impl::Slot* {
    for (Impl::Slot& slot : camera.slots) {
      if (slot.allocated && !slot.in_use && !slot.quarantined) {
        return &slot;
      }
    }
    return nullptr;
  };
  while (!stop_requested.load(std::memory_order_acquire)) {
    if (Impl::Slot* slot = find_available_slot()) {
      slot->in_use = true;
      slot->public_slot.submitted_to_hardware = false;
      return &slot->public_slot;
    }
    // Recorder满载时相机回调等待自有NV12槽回收，避免把短暂JPU背压升级为fatal。
    impl_->slot_condition.wait(lock, [&] {
      return stop_requested.load(std::memory_order_acquire) ||
             find_available_slot() != nullptr;
    });
  }
  return nullptr;
}

void X5JpegEncoder::NotifySlotWaiters() noexcept {
  if (impl_ != nullptr) {
    impl_->slot_condition.notify_all();
  }
}

void X5JpegEncoder::ReleaseSlot(X5JpegInputSlot* public_slot) noexcept {
  if (public_slot == nullptr || impl_ == nullptr) {
    return;
  }
  const int camera_id = public_slot->camera_id;
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    return;
  }
  bool released = false;
  {
    std::lock_guard<std::mutex> lock(impl_->slot_mutex);
    for (Impl::Slot& slot : impl_->cameras[static_cast<size_t>(camera_id)].slots) {
      if (&slot.public_slot == public_slot) {
        slot.public_slot.submitted_to_hardware = false;
        if (!slot.quarantined) {
          slot.in_use = false;
          released = true;
        }
        break;
      }
    }
  }
  if (released) {
    impl_->slot_condition.notify_one();
  }
}

void X5JpegEncoder::QuarantineSlot(X5JpegInputSlot* public_slot) noexcept {
  if (public_slot == nullptr || impl_ == nullptr) {
    return;
  }
  const int camera_id = public_slot->camera_id;
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->slot_mutex);
  for (Impl::Slot& slot : impl_->cameras[static_cast<size_t>(camera_id)].slots) {
    if (&slot.public_slot == public_slot) {
      slot.quarantined = true;
      slot.in_use = true;
      return;
    }
  }
}

X5JpegNv12CopyResult X5JpegEncoder::CopyNv12ToSlot(const QueuedFrame& frame,
                                                   X5JpegInputSlot* slot) {
  if (slot == nullptr || frame.y_data == nullptr || frame.uv_data == nullptr ||
      frame.width != slot->width || frame.height != slot->height ||
      frame.stride < frame.width || frame.vstride < frame.height ||
      slot->stride < frame.width || slot->vstride < frame.height) {
    throw std::runtime_error("hardware JPEG invalid NV12 staging input");
  }
  const uint64_t y_required = static_cast<uint64_t>(frame.stride) * frame.vstride;
  const uint64_t uv_required = static_cast<uint64_t>(frame.stride) * (frame.vstride / 2U);
  if (frame.y_size < y_required || frame.uv_size < uv_required) {
    throw std::runtime_error("hardware JPEG source plane capacity too small");
  }
  X5JpegNv12CopyResult copy_result;
  const bool contiguous_rows = frame.stride == frame.width && slot->stride == slot->width;
  CopyNv12Plane(static_cast<const uint8_t*>(frame.y_data), slot->y_data, frame.width,
                frame.height, frame.stride, slot->stride, contiguous_rows, &copy_result);
  CopyNv12Plane(static_cast<const uint8_t*>(frame.uv_data), slot->uv_data, frame.width,
                frame.height / 2U, frame.stride, slot->stride, contiguous_rows, &copy_result);
  int result = hb_mem_flush_buf_with_vaddr(
      reinterpret_cast<uint64_t>(slot->y_data), slot->y_capacity);
  if (result != 0) {
    throw std::runtime_error(StageError(slot->camera_id, "flush_y", result));
  }
  result = hb_mem_flush_buf_with_vaddr(
      reinterpret_cast<uint64_t>(slot->uv_data), slot->uv_capacity);
  if (result != 0) {
    throw std::runtime_error(StageError(slot->camera_id, "flush_uv", result));
  }
  return copy_result;
}


void X5JpegEncoder::Encode(const X5JpegEncodeRequest& request, std::vector<uint8_t>* jpeg) {
  if (jpeg == nullptr) {
    throw std::runtime_error("hardware JPEG invalid encode request");
  }
  jpeg->clear();
  size_t jpeg_size = 0U;
  EncodeAppend(request, jpeg, &jpeg_size);
}

void X5JpegEncoder::EncodeAppend(const X5JpegEncodeRequest& request,
                                 std::vector<uint8_t>* payload,
                                 size_t* jpeg_size) {
  if (!started_ || payload == nullptr || jpeg_size == nullptr || request.slot == nullptr ||
      request.camera_id < 0 || request.camera_id >= kMaxChannels) {
    throw std::runtime_error("hardware JPEG invalid encode request");
  }
  *jpeg_size = 0U;
  Impl::Camera& camera = impl_->cameras[static_cast<size_t>(request.camera_id)];
  media_codec_buffer_t input{};
  int result = hb_mm_mc_dequeue_input_buffer(&camera.context, &input, kCodecTimeoutMs);
  if (result != 0) {
    throw std::runtime_error(StageError(request.camera_id, "dequeue_input", result));
  }
  media_codec_buffer_t queued_input = input;
  mc_video_frame_buffer_info_t& frame = queued_input.vframe_buf;
  frame.vir_ptr[0] = request.slot->y_data;
  frame.vir_ptr[1] = request.slot->uv_data;
  frame.vir_ptr[2] = nullptr;
  frame.phy_ptr[0] = request.slot->y_phys;
  frame.phy_ptr[1] = request.slot->uv_phys;
  frame.phy_ptr[2] = 0U;
  frame.fd[0] = request.slot->y_fd;
  frame.fd[1] = request.slot->uv_fd;
  frame.fd[2] = -1;
  frame.compSize[0] = static_cast<hb_u32>(request.slot->y_capacity);
  frame.compSize[1] = static_cast<hb_u32>(request.slot->uv_capacity);
  frame.compSize[2] = 0U;
  frame.width = static_cast<hb_s32>(request.slot->width);
  frame.height = static_cast<hb_s32>(request.slot->height);
  frame.pix_fmt = MC_PIXEL_FORMAT_NV12;
  frame.stride = static_cast<hb_s32>(request.slot->stride);
  frame.vstride = static_cast<hb_s32>(request.slot->stride);
  frame.size = static_cast<hb_u32>(request.slot->y_capacity + request.slot->uv_capacity);
  frame.pts = request.pts_us;
  queued_input.type = MC_VIDEO_FRAME_BUFFER;
  const_cast<X5JpegInputSlot*>(request.slot)->submitted_to_hardware = true;
  result = hb_mm_mc_queue_input_buffer(&camera.context, &queued_input, kCodecTimeoutMs);
  if (result != 0) {
    throw std::runtime_error(StageError(request.camera_id, "queue_input", result));
  }

  const size_t jpeg_start = payload->size();
  for (size_t output_count = 0U; output_count < kMaxJpegOutputBuffers; ++output_count) {
    media_codec_buffer_t output{};
    media_codec_output_buffer_info_t info{};
    result = hb_mm_mc_dequeue_output_buffer(&camera.context, &output, &info, kCodecTimeoutMs);
    if (result != 0) {
      throw std::runtime_error(StageError(request.camera_id, "dequeue_output", result));
    }
    bool output_return_attempted = false;
    try {
      const size_t output_size = static_cast<size_t>(output.vstream_buf.size);
      const size_t current_jpeg_size = payload->size() - jpeg_start;
      if (output_size == 0U) {
        throw std::runtime_error("hardware JPEG malformed output: missing payload size");
      }
      if (output_size > impl_->bitstream_capacity ||
          current_jpeg_size > impl_->bitstream_capacity - output_size) {
        throw std::runtime_error("hardware JPEG malformed output: payload size exceeds bounds");
      }
      if (output.vstream_buf.vir_ptr == nullptr) {
        throw std::runtime_error("hardware JPEG malformed output: missing SOI");
      }
      payload->insert(payload->end(), output.vstream_buf.vir_ptr,
                      output.vstream_buf.vir_ptr + output_size);
      size_t payload_size = 0U;
      const bool complete = TryResolveJpegPayloadLength(
          payload->data() + jpeg_start, payload->size() - jpeg_start,
          impl_->bitstream_capacity, &payload_size);
      const bool last_slice = IsLastJpegOutputSlice(info);
      if (!complete && last_slice) {
        throw std::runtime_error("hardware JPEG malformed output: missing EOI");
      }
      output_return_attempted = true;
      result = hb_mm_mc_queue_output_buffer(&camera.context, &output, kCodecTimeoutMs);
      if (result != 0) {
        throw std::runtime_error(StageError(request.camera_id, "queue_output", result));
      }
      if (complete) {
        payload->resize(jpeg_start + payload_size);
        *jpeg_size = payload_size;
        const_cast<X5JpegInputSlot*>(request.slot)->submitted_to_hardware = false;
        return;
      }
      // 同一 JPEG 可能由多段 Media Codec 输出组成；未见 EOI 前继续收集下一段。
    } catch (...) {
      if (!output_return_attempted) {
        output_return_attempted = true;
        (void)hb_mm_mc_queue_output_buffer(&camera.context, &output, kCodecTimeoutMs);
      }
      throw;
    }
  }
  throw std::runtime_error("hardware JPEG malformed output: too many output buffers");

}

}  // namespace robobaton_demo
