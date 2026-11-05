// Linux UVC gadget ABI: kernel/include/uapi/linux/usb/{g_uvc,video}.h.
// MediaCodec ownership/layout follows the X5 SDK JPEG feeder/reader and
// RoboBaton_4p_demo/src/{mosaic_nv12,x5_jpeg_encoder}.cpp.
#include "sc132camera.h"
#include <hb_media_codec.h>
#include <linux/videodev2.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr uint32_t kWidth = 1280, kHeight = 960;
constexpr uint32_t kTileWidth = 640, kTileHeight = 480;
constexpr uint32_t kMaxFrame = kWidth * kHeight * 3 / 2;
constexpr uint32_t kInterval = 333333, kPayload = 3072;
constexpr int kCodecTimeoutMs = 500;
constexpr size_t kBufferCount = 3;
constexpr const char* kFunction = "/sys/kernel/config/usb_gadget/4p_usb/functions/uvc.0";

uint64_t MonotonicNs() noexcept {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}
[[noreturn]] void Fail(const std::string& stage) {
  throw std::runtime_error(stage + ": " + std::strerror(errno));
}
void CheckCodec(int result, const char* stage) {
  if (result != 0) throw std::runtime_error(std::string(stage) + " result=" + std::to_string(result));
}
int Ioctl(int fd, unsigned long request, void* arg) noexcept {
  int result;
  do { result = ioctl(fd, request, arg); } while (result < 0 && errno == EINTR);
  return result;
}
bool CleanupIoctl(int fd, unsigned long request, void* arg, const char* stage) noexcept {
  if (Ioctl(fd, request, arg) == 0) return true;
  std::fprintf(stderr, "cleanup: %s: %s\n", stage, std::strerror(errno));
  return false;
}
std::string ReadWord(const std::string& path) {
  std::ifstream input(path);
  std::string value;
  if (!(input >> value)) throw std::runtime_error("cannot read " + path);
  return value;
}
uint32_t ReadNumber(const std::string& path) {
  const std::string text = ReadWord(path);
  size_t consumed = 0;
  const unsigned long value = std::stoul(text, &consumed, 0);
  if (consumed != text.size() || value > UINT32_MAX) throw std::runtime_error("invalid " + path);
  return static_cast<uint32_t>(value);
}

// The manager preflights owners too. Fail closed here even for unfamiliar
// executables loading the producer; never stop a service or signal its owner.
void RefuseExistingOwners() {
  DIR* proc = opendir("/proc");
  if (proc == nullptr) Fail("opendir /proc");
  try {
    while (dirent* entry = readdir(proc)) {
      const std::string pid = entry->d_name;
      if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos ||
          pid == std::to_string(getpid())) continue;
      const std::string base = "/proc/" + pid;
      std::array<char, 4096> exe{};
      const ssize_t count = readlink((base + "/exe").c_str(), exe.data(), exe.size() - 1);
      if (count < 0) {
        // Kernel threads and exited tasks have no executable link. Permission
        // errors are not evidence that a possible acquisition owner is absent.
        if (errno == ENOENT || errno == ESRCH) continue;
        Fail("readlink " + base + "/exe");
      }
      const std::string path(exe.data(), static_cast<size_t>(count));
      std::string name = path.substr(path.find_last_of('/') + 1);
      const size_t deleted = name.find(" (deleted)");
      if (deleted != std::string::npos) name.resize(deleted);
      if (name == "sensor_demo" || name == "sensor_web" || name == "cam_demo" ||
          name == "mosaic_rtsp_demo" || name == "robobaton_sensors_node" ||
          name == "x5-uvc-mosaic") {
        throw std::runtime_error("acquisition owner present pid=" + pid + " executable=" + path);
      }
      errno = 0;
      std::ifstream maps(base + "/maps");
      if (!maps) {
        if (errno == ENOENT || errno == ESRCH) continue;
        throw std::runtime_error("cannot inspect acquisition owner " + base + "/maps");
      }
      std::string line;
      while (std::getline(maps, line)) {
        if (line.find("/libsc132.so") != std::string::npos) {
          throw std::runtime_error("producer library already loaded pid=" + pid + " executable=" + path);
        }
      }
    }
  } catch (...) { closedir(proc); throw; }
  closedir(proc);
}

struct Group {
  std::array<sc132_frame_t*, 4> frames{};
  uint64_t timestamp_ns = 0;
  Group() = default;
  Group(const Group&) = delete;
  Group& operator=(const Group&) = delete;
  ~Group() { Clear(); }
  void Clear() noexcept {
    for (auto*& frame : frames) {
      if (frame != nullptr) sc132_frame_release(std::exchange(frame, nullptr));
    }
  }
  void Swap(Group& other) noexcept {
    frames.swap(other.frames);
    std::swap(timestamp_ns, other.timestamp_ns);
  }
};

class Source {
 public:
  explicit Source(int wake_fd) : wake_fd_(wake_fd) {}
  ~Source() { Stop(); }
  void Start() {
    RefuseExistingOwners();
    CheckCodec(sc132_set_fps(30), "sc132_set_fps");
    // Same 90-degree mounting compensation as the installed non-ROS demos.
    CheckCodec(sc132_set_output_rotation(90), "sc132_set_output_rotation");
    sc132_frame_set_config_t config = SC132_FRAME_SET_CONFIG_INIT;
    config.callback = Callback;
    config.user_data = this;
    config.camera_count = 4;
    config.width = kTileWidth;
    config.height = kTileHeight;
    config.timeout_ms = 100;
    last_group_ns_.store(MonotonicNs());
    attempted_ = true;  // STARTUP_FAILED can still own threads: always drain.
    CheckCodec(sc132_start_frame_set(&config, 0x0f), "sc132_start_frame_set");
    // Startup itself can take several seconds; the runtime stall grace begins
    // after start returns, without claiming any frame has been observed.
    last_group_ns_.store(MonotonicNs());
  }
  void SetStreaming(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    streaming_ = enabled;
    if (!enabled) pending_.Clear();
  }
  bool Take(Group& group) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.frames[0] == nullptr) return false;
    group.Swap(pending_);
    return true;
  }
  bool failed() const noexcept { return failed_.load(); }
  void CheckHealth() const {
    if (failed_.load()) throw std::runtime_error("SC132 callback rejected malformed group or retain failed");
    const uint64_t last = last_group_ns_.load();
    const uint64_t now = MonotonicNs();
    if (now == 0 || last == 0 || now - last > 3000000000ULL)
      throw std::runtime_error("SC132 acquisition stalled: no complete four-camera group for 3 seconds");
  }
  bool Stop() noexcept {
    if (!attempted_) return true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      streaming_ = false;
    }
    sc132_request_stop();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.Clear();
    }
    // Retained producer frames are gone before blocking quiescence. The second
    // stop is the producer's documented cleanup retry, not a restart or takeover.
    sc132_stop();
    sc132_stop();
    // The public stop API is void. An intentionally invalid config value checks
    // its documented active-state-first gate without changing stopped config.
    if (sc132_set_fps(UINT32_MAX) != SC132_STATUS_INVALID_ARGUMENT) {
      std::fprintf(stderr, "fatal: SC132 remains non-quiescent after blocking stop/retry\n");
      return false;
    }
    attempted_ = false;
    std::fprintf(stderr, "source stopped: received=%llu whole_group_drops=%llu\n",
                 static_cast<unsigned long long>(received_), static_cast<unsigned long long>(dropped_));
    return true;
  }
 private:
  static void Callback(const sc132_frame_set_t* set, void* user) noexcept {
    auto* self = static_cast<Source*>(user);
    try {
      if (set == nullptr) throw std::runtime_error("null group");
      self->Receive(*set);
    } catch (...) {
      self->failed_.store(true);
      self->Wake();
    }
  }
  void Wake() noexcept {
    const uint64_t one = 1;
    ssize_t result;
    do { result = write(wake_fd_, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) failed_.store(true);
  }
  void Receive(const sc132_frame_set_t& set) {
    if (set.struct_size != sizeof(set) || set.camera_count != 4 || set.group_timestamp_ns == 0 ||
        set.max_skew_ns > SC132_FRAME_SET_DEFAULT_MAX_SKEW_NS)
      throw std::runtime_error("invalid group size");
    std::array<bool, 4> seen{};
    for (const auto& item : set.items) {
      if (item.frame == nullptr || item.camera_id >= 4 || seen[item.camera_id] ||
          item.width != kTileWidth || item.height != kTileHeight)
        throw std::runtime_error("invalid group item");
      seen[item.camera_id] = true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    ++received_;
    last_group_ns_.store(MonotonicNs());
    if (!streaming_) return;
    // Latest complete group replaces the pending group atomically; no unbounded
    // queue and no independently dropped tile can mix exposure groups.
    Group incoming;
    for (const auto& item : set.items) {
      if (sc132_frame_retain(item.frame) != SC132_STATUS_OK)
        throw std::runtime_error("retain failed");
      incoming.frames[item.camera_id] = item.frame;
    }
    incoming.timestamp_ns = set.group_timestamp_ns;
    if (pending_.frames[0] != nullptr) ++dropped_;
    incoming.Swap(pending_);
    Wake();
  }
  int wake_fd_;
  bool attempted_ = false, streaming_ = false, stopping_ = false;
  std::mutex mutex_;
  Group pending_;
  std::atomic<bool> failed_{false};
  std::atomic<uint64_t> last_group_ns_{0};
  uint64_t received_ = 0, dropped_ = 0;
};

void Compose(const Group& group, mc_video_frame_buffer_info_t& output) {
  // MediaCodec vstride is CHROMA ROW STRIDE, unlike SC132 vstride (row count).
  if (output.width != static_cast<int>(kWidth) || output.height != static_cast<int>(kHeight) ||
      output.pix_fmt != MC_PIXEL_FORMAT_NV12 || output.vir_ptr[0] == nullptr ||
      output.vir_ptr[1] == nullptr || output.stride < static_cast<int>(kWidth) ||
      output.vstride < static_cast<int>(kWidth) ||
      output.compSize[0] < static_cast<uint64_t>(output.stride) * kHeight ||
      output.compSize[1] < static_cast<uint64_t>(output.vstride) * (kHeight / 2))
    throw std::runtime_error("invalid codec-owned NV12 plane geometry");
  std::array<sc132_frame_info_t, 4> sources{};
  for (size_t camera = 0; camera < sources.size(); ++camera) {
    auto& info = sources[camera];
    info.struct_size = sizeof(info);
    if (sc132_frame_get_info(group.frames[camera], &info) != SC132_STATUS_OK ||
        info.struct_size != sizeof(info) || info.camera_id != camera ||
        info.width != kTileWidth || info.height != kTileHeight ||
        info.stride < kTileWidth || info.vstride < kTileHeight || (info.vstride & 1) != 0 ||
        info.y_data == nullptr || info.uv_data == nullptr ||
        info.y_size < static_cast<uint64_t>(info.stride) * info.vstride ||
        info.uv_size < static_cast<uint64_t>(info.stride) * (info.vstride / 2))
      throw std::runtime_error("invalid SC132 NV12 tile geometry");
  }
  for (size_t camera = 0; camera < sources.size(); ++camera) {
    const auto& info = sources[camera];
    const size_t x = (camera % 2) * kTileWidth, y = (camera / 2) * kTileHeight;
    for (size_t row = 0; row < kTileHeight; ++row)
      std::memcpy(output.vir_ptr[0] + (y + row) * output.stride + x,
                  static_cast<const uint8_t*>(info.y_data) + row * info.stride, kTileWidth);
    for (size_t row = 0; row < kTileHeight / 2; ++row)
      std::memcpy(output.vir_ptr[1] + (y / 2 + row) * output.vstride + x,
                  static_cast<const uint8_t*>(info.uv_data) + row * info.stride, kTileWidth);
  }
}

// Parse marker lengths before scanning entropy bytes: the JPU's APP9 contains
// an arbitrary frame index, which can itself equal FF D9 and is NOT an EOI.
class JpegEnd {
 public:
  size_t Parse(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    if (data[0] != 0xff || data[1] != 0xd8) throw std::runtime_error("hardware JPEG has no SOI");
    while (position_ < size) {
      if (entropy_) {
        while (position_ < size && data[position_] != 0xff) ++position_;
        if (position_ == size) return 0;
      }
      const size_t start = position_;
      if (data[start] != 0xff) throw std::runtime_error("invalid JPEG marker");
      size_t code_at = start + 1;
      while (code_at < size && data[code_at] == 0xff) ++code_at;
      if (code_at == size) return 0;
      const uint8_t code = data[code_at];
      if (entropy_ && (code == 0 || (code >= 0xd0 && code <= 0xd7))) {
        position_ = code_at + 1;
        continue;
      }
      entropy_ = false;
      if (code == 0xd9) return code_at + 1;
      if (code == 0 || code == 0xd8) throw std::runtime_error("unexpected JPEG marker");
      if (code == 1 || (code >= 0xd0 && code <= 0xd7)) { position_ = code_at + 1; continue; }
      if (size - code_at < 3) return 0;
      const size_t length = static_cast<size_t>(data[code_at + 1]) * 256 + data[code_at + 2];
      if (length < 2) throw std::runtime_error("invalid JPEG marker length");
      if (length > size - code_at - 1) return 0;
      position_ = code_at + 1 + length;
      entropy_ = code == 0xda;
    }
    return 0;
  }
 private:
  size_t position_ = 2;
  bool entropy_ = false;
};

class Encoder {
 public:
  ~Encoder() { Close(); }
  void Open() {
    CheckCodec(hb_mm_mc_get_default_context(MEDIA_CODEC_ID_JPEG, true, &context_), "JPEG default context");
    context_.codec_id = MEDIA_CODEC_ID_JPEG;
    context_.encoder = true;
    auto& params = context_.video_enc_params;
    params.width = kWidth;
    params.height = kHeight;
    params.pix_fmt = MC_PIXEL_FORMAT_NV12;
    params.frame_buf_count = 1;
    params.external_frame_buf = false;
    params.bitstream_buf_count = 1;
    params.bitstream_buf_size = kMaxFrame;  // 4096-aligned for this fixed profile.
    params.jpeg_enc_config.quality_factor = 75;
    params.jpeg_enc_config.restart_interval = kWidth / 16;
    params.jpeg_enc_config.extended_sequential = false;
    CheckCodec(hb_mm_mc_initialize(&context_), "JPEG initialize");
    initialized_ = true;
    CheckCodec(hb_mm_mc_configure(&context_), "JPEG configure");
    mc_av_codec_startup_params_t startup{};
    startup.video_enc_startup_params.receive_frame_number = 0;
    CheckCodec(hb_mm_mc_start(&context_, &startup), "JPEG start");
    started_ = true;
  }
  size_t Encode(Group& group, uint8_t* destination, size_t capacity) {
    media_codec_buffer_t input{};
    CheckCodec(hb_mm_mc_dequeue_input_buffer(&context_, &input, kCodecTimeoutMs), "JPEG dequeue input");
    if (input.type != MC_VIDEO_FRAME_BUFFER) throw std::runtime_error("unexpected JPEG input type");
    Compose(group, input.vframe_buf);
    input.vframe_buf.pts = group.timestamp_ns / 1000;
    // Nothing borrowed from SC132 is ever queued into the codec.
    group.Clear();
    CheckCodec(hb_mm_mc_queue_input_buffer(&context_, &input, kCodecTimeoutMs), "JPEG queue input");
    size_t size = 0;
    JpegEnd jpeg_end;
    for (size_t slice = 0; slice < 64; ++slice) {
      media_codec_buffer_t output{};
      media_codec_output_buffer_info_t info{};
      CheckCodec(hb_mm_mc_dequeue_output_buffer(&context_, &output, &info, kCodecTimeoutMs), "JPEG dequeue output");
      bool return_attempted = false;
      try {
        const size_t bytes = output.vstream_buf.size;
        if (output.type != MC_VIDEO_STREAM_BUFFER || output.vstream_buf.vir_ptr == nullptr ||
            bytes == 0 || bytes > capacity - size)
          throw std::runtime_error("invalid or oversized JPEG output");
        std::memcpy(destination + size, output.vstream_buf.vir_ptr, bytes);
        size += bytes;
        const size_t complete = jpeg_end.Parse(destination, size);
        const bool last = info.jpeg_stream_info.slice_num != 0 &&
            info.jpeg_stream_info.slice_idx >= info.jpeg_stream_info.slice_num - 1;
        if (complete == 0 && last) throw std::runtime_error("hardware JPEG has no EOI");
        return_attempted = true;
        CheckCodec(hb_mm_mc_queue_output_buffer(&context_, &output, kCodecTimeoutMs), "JPEG return output");
        if (complete != 0) return complete;
      } catch (...) {
        if (!return_attempted) {
          const int result = hb_mm_mc_queue_output_buffer(&context_, &output, kCodecTimeoutMs);
          if (result != 0) std::fprintf(stderr, "cleanup: JPEG return output result=%d\n", result);
        }
        throw;
      }
    }
    throw std::runtime_error("hardware JPEG exceeded 64 output slices");
  }
  bool Close() noexcept {
    bool ok = true;
    if (started_) {
      const int result = hb_mm_mc_stop(&context_);
      if (result != 0) { std::fprintf(stderr, "cleanup: JPEG stop result=%d\n", result); ok = false; }
      else started_ = false;
    }
    if (initialized_) {
      const int result = hb_mm_mc_release(&context_);
      if (result != 0) { std::fprintf(stderr, "cleanup: JPEG release result=%d\n", result); ok = false; }
      else { initialized_ = false; started_ = false; }
    }
    return ok;
  }
 private:
  media_codec_context_t context_{};
  bool initialized_ = false, started_ = false;
};

uvc_streaming_control FixedControl() noexcept {
  uvc_streaming_control control{};
  control.bmHint = htole16(1);
  control.bFormatIndex = 1;
  control.bFrameIndex = 1;
  control.dwFrameInterval = htole32(kInterval);
  control.dwMaxVideoFrameSize = htole32(kMaxFrame);
  control.dwMaxPayloadTransferSize = htole32(kPayload);
  control.dwClockFrequency = htole32(48000000);
  control.bmFramingInfo = 3;
  control.bPreferedVersion = 1;
  control.bMinVersion = 1;
  control.bMaxVersion = 1;
  return control;
}

class Gadget {
 public:
  ~Gadget() { Close(); }
  int fd() const noexcept { return fd_; }
  bool streaming() const noexcept { return streaming_; }
  bool has_queued() const noexcept { return queued_ != 0; }
  void Open(const std::string& device) {
    const std::string function = kFunction;
    if (ReadWord("/sys/kernel/config/usb_gadget/4p_usb/UDC") != "35100000.usb")
      throw std::runtime_error("4p_usb is not bound to 35100000.usb");
    control_interface_ = ReadNumber(function + "/control/bInterfaceNumber");
    streaming_interface_ = ReadNumber(function + "/streaming/bInterfaceNumber");
    if (control_interface_ > 255 || streaming_interface_ > 255 || control_interface_ == streaming_interface_ ||
        ReadNumber(function + "/streaming_interval") != 1 ||
        ReadNumber(function + "/streaming_maxpacket") != kPayload ||
        ReadNumber(function + "/streaming_maxburst") != 0)
      throw std::runtime_error("UVC ConfigFS interface/endpoint does not match fixed profile");
    fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) Fail("open " + device);
    struct stat status{};
    if (fstat(fd_, &status) != 0) Fail("fstat UVC device");
    if (!S_ISCHR(status.st_mode)) throw std::runtime_error("UVC device is not a character device");
    v4l2_capability cap{};
    if (Ioctl(fd_, VIDIOC_QUERYCAP, &cap) != 0) Fail("VIDIOC_QUERYCAP");
    const uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (std::string(reinterpret_cast<char*>(cap.driver)) != "g_uvc" ||
        (caps & (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING)) !=
        (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING))
      throw std::runtime_error("device is not a streaming UVC gadget output");
    // Validate the node's kernel-exported ConfigFS instance, not merely its name.
    const std::string node = device.substr(device.find_last_of('/') + 1);
    if (ReadWord("/sys/class/video4linux/" + node + "/function_name") != "uvc.0")
      throw std::runtime_error("video node is not function uvc.0");
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = kWidth;
    format.fmt.pix.height = kHeight;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.sizeimage = kMaxFrame;
    if (Ioctl(fd_, VIDIOC_S_FMT, &format) != 0) Fail("VIDIOC_S_FMT MJPEG");
    if (format.fmt.pix.width != kWidth || format.fmt.pix.height != kHeight ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG || format.fmt.pix.sizeimage != kMaxFrame)
      throw std::runtime_error("UVC descriptor format/sizeimage differs from 1280x960 MJPEG profile");
    for (uint32_t type : {UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT,
                          UVC_EVENT_DATA, UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF}) {
      v4l2_event_subscription subscription{};
      subscription.type = type;
      if (Ioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription) != 0) Fail("VIDIOC_SUBSCRIBE_EVENT");
    }
  }
  void Activate() {
    // SETUP subscription activates the ConfigFS UVC function in this kernel.
    // Do it last, so slow sensor startup cannot starve host control requests.
    v4l2_event_subscription subscription{};
    subscription.type = UVC_EVENT_SETUP;
    if (Ioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription) != 0) Fail("subscribe UVC SETUP/activate");
  }
  void Events(Source& source) {
    for (;;) {
      v4l2_event event{};
      if (Ioctl(fd_, VIDIOC_DQEVENT, &event) != 0) {
        if (errno == EAGAIN) return;
        Fail("VIDIOC_DQEVENT");
      }
      uvc_event data{};
      static_assert(sizeof(data) <= sizeof(event.u.data), "UVC event ABI size");
      std::memcpy(&data, event.u.data, sizeof(data));
      switch (event.type) {
        case UVC_EVENT_CONNECT:
          if (data.speed < USB_SPEED_HIGH) throw std::runtime_error("MJPEG profile requires USB high/super speed");
          std::fprintf(stderr, "UVC connected speed=%d (not streaming proof)\n", data.speed);
          break;
        case UVC_EVENT_DISCONNECT:
          committed_ = false;
          [[fallthrough]];
        case UVC_EVENT_STREAMOFF:
          source.SetStreaming(false);
          if (!StopStream()) throw std::runtime_error("UVC stream teardown failed");
          break;
        case UVC_EVENT_STREAMON:
          if (!committed_) throw std::runtime_error("UVC STREAMON without valid COMMIT");
          StartStream();
          source.SetStreaming(true);
          break;
        case UVC_EVENT_SETUP: Setup(data.req); break;
        case UVC_EVENT_DATA: Data(data.data); break;
        default: throw std::runtime_error("unexpected UVC event");
      }
    }
  }
  int FreeBuffer() {
    if (!streaming_) return -1;
    while (queued_ != 0) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (Ioctl(fd_, VIDIOC_DQBUF, &buffer) != 0) {
        if (errno == EAGAIN) break;
        Fail("VIDIOC_DQBUF");
      }
      if (buffer.index >= count_ || !buffers_[buffer.index].queued)
        throw std::runtime_error("invalid UVC dequeued buffer index");
      buffers_[buffer.index].queued = false;
      --queued_;
      if ((buffer.flags & V4L2_BUF_FLAG_ERROR) != 0)
        std::fprintf(stderr, "UVC returned output buffer with transport error\n");
    }
    for (size_t i = 0; i < count_; ++i) if (!buffers_[i].queued) return static_cast<int>(i);
    return -1;
  }
  void Send(int index, Group& group, Encoder& encoder) {
    auto& mapped = buffers_[static_cast<size_t>(index)];
    const uint64_t timestamp = group.timestamp_ns;
    const size_t size = encoder.Encode(group, static_cast<uint8_t*>(mapped.address), kMaxFrame);
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = static_cast<uint32_t>(index);
    buffer.bytesused = static_cast<uint32_t>(size);
    buffer.field = V4L2_FIELD_NONE;
    buffer.timestamp.tv_sec = timestamp / 1000000000ULL;
    buffer.timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000;
    if (Ioctl(fd_, VIDIOC_QBUF, &buffer) != 0) Fail("VIDIOC_QBUF");
    mapped.queued = true;
    ++queued_;
    ++sent_;
  }
  bool Close() noexcept {
    bool ok = StopStream();
    if (fd_ >= 0) {
      if (close(std::exchange(fd_, -1)) != 0) { std::perror("cleanup: close UVC"); ok = false; }
      // close is the kernel ownership barrier even after STREAMOFF failure;
      // only then may the remaining mappings be released.
      streaming_ = false;
      allocated_ = false;
      ok = StopStream() && ok;
      std::fprintf(stderr, "UVC stopped: queued_real_frames=%llu (not host receiving proof)\n",
                   static_cast<unsigned long long>(sent_));
    }
    return ok;
  }
 private:
  void StartStream() {
    if (streaming_) return;
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = kBufferCount;
    if (Ioctl(fd_, VIDIOC_REQBUFS, &request) != 0) Fail("VIDIOC_REQBUFS");
    allocated_ = true;
    if (request.count == 0 || request.count > kBufferCount) throw std::runtime_error("invalid UVC buffer count");
    count_ = request.count;
    for (size_t i = 0; i < count_; ++i) {
      v4l2_buffer buffer{};
      buffer.type = request.type;
      buffer.memory = request.memory;
      buffer.index = static_cast<uint32_t>(i);
      if (Ioctl(fd_, VIDIOC_QUERYBUF, &buffer) != 0) Fail("VIDIOC_QUERYBUF");
      if (buffer.length < kMaxFrame) throw std::runtime_error("UVC output buffer too small");
      buffers_[i].length = buffer.length;
      buffers_[i].address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
      if (buffers_[i].address == MAP_FAILED) Fail("mmap UVC output");
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    // Completes the gadget's delayed SET_INTERFACE status immediately. No fake
    // priming frame: the kernel waits for the first real composed JPEG.
    if (Ioctl(fd_, VIDIOC_STREAMON, &type) != 0) Fail("VIDIOC_STREAMON");
    streaming_ = true;
  }
  bool StopStream() noexcept {
    bool ok = true;
    if (streaming_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      ok = CleanupIoctl(fd_, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
      // Closing fd below is the final ownership barrier if STREAMOFF failed.
      if (!ok) return false;
      streaming_ = false;
    }
    for (auto& buffer : buffers_) {
      if (buffer.address != MAP_FAILED) {
        if (munmap(buffer.address, buffer.length) != 0) { std::perror("cleanup: munmap UVC"); ok = false; }
        else buffer.address = MAP_FAILED;
      }
      buffer.queued = false;
    }
    queued_ = 0;
    count_ = 0;
    if (allocated_) {
      v4l2_requestbuffers request{};
      request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      request.memory = V4L2_MEMORY_MMAP;
      const bool released = CleanupIoctl(fd_, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS(0)");
      ok = released && ok;
      if (released) allocated_ = false;
    }
    return ok;
  }
  void Setup(const usb_ctrlrequest& request) {
    pending_control_ = 0;
    const uint16_t length = le16toh(request.wLength);
    const uint16_t index = le16toh(request.wIndex);
    const uint8_t selector = le16toh(request.wValue) >> 8;
    const bool input = (request.bRequestType & USB_DIR_IN) != 0;
    uvc_request_data response{};
    response.length = -EL2HLT;
    if ((request.bRequestType & (USB_TYPE_MASK | USB_RECIP_MASK)) == (USB_TYPE_CLASS | USB_RECIP_INTERFACE)) {
      if ((index & 0xff) == streaming_interface_ && (index >> 8) == 0 &&
          (selector == UVC_VS_PROBE_CONTROL || selector == UVC_VS_COMMIT_CONTROL)) {
        if (request.bRequest == UVC_SET_CUR && !input && (length == 26 || length == 34)) {
          pending_control_ = selector;
          pending_length_ = length;
          response.length = length;
        } else if (input) {
          uvc_streaming_control control = FixedControl();
          switch (request.bRequest) {
            case UVC_GET_CUR:
            case UVC_GET_MIN:
            case UVC_GET_MAX:
            case UVC_GET_DEF:
              std::memcpy(response.data, &control, sizeof(control));
              response.length = sizeof(control);
              break;
            case UVC_GET_RES: response.length = sizeof(control); break;
            case UVC_GET_LEN:
              response.data[0] = sizeof(control); response.data[1] = 0; response.length = 2; break;
            case UVC_GET_INFO: response.data[0] = 3; response.length = 1; break;
            default: break;
          }
        }
      } else if ((index & 0xff) == control_interface_ && (index >> 8) == 0 &&
                 selector == UVC_VC_REQUEST_ERROR_CODE_CONTROL && input && request.bRequest == UVC_GET_CUR) {
        response.data[0] = request_error_;
        response.length = 1;
        request_error_ = 0;
      }
    }
    if (response.length < 0) request_error_ = 0x06;  // Unsupported control: stall, never pretend to apply it.
    else response.length = std::min<int>(response.length, length);
    if (Ioctl(fd_, UVCIOC_SEND_RESPONSE, &response) != 0) Fail("UVCIOC_SEND_RESPONSE");
  }
  void Data(const uvc_request_data& data) {
    if (pending_control_ == 0 || data.length != pending_length_) {
      request_error_ = 0x07;
      pending_control_ = 0;
      throw std::runtime_error("invalid UVC negotiation DATA stage");
    }
    uvc_streaming_control control{};
    std::memcpy(&control, data.data, static_cast<size_t>(data.length));
    const uint8_t selector = std::exchange(pending_control_, 0);
    // PROBE normalizes the request to the sole advertised profile. COMMIT must
    // name that profile; unsupported host choices never silently change it.
    if (selector == UVC_VS_COMMIT_CONTROL) {
      if (control.bFormatIndex != 1 || control.bFrameIndex != 1 || le32toh(control.dwFrameInterval) != kInterval) {
        request_error_ = 0x04;
        throw std::runtime_error("host committed unsupported UVC profile");
      }
      committed_ = true;
    }
  }
  struct Buffer { void* address = MAP_FAILED; size_t length = 0; bool queued = false; };
  int fd_ = -1;
  uint32_t control_interface_ = 0, streaming_interface_ = 0;
  std::array<Buffer, kBufferCount> buffers_{};
  size_t count_ = 0, queued_ = 0;
  bool streaming_ = false, allocated_ = false, committed_ = false;
  uint8_t pending_control_ = 0, request_error_ = 0;
  uint16_t pending_length_ = 0;
  uint64_t sent_ = 0;
};

class ReadyFile {
 public:
  ~ReadyFile() { Remove(); }
  void Create(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) Fail("create ready file " + path);
    path_ = path;
    const std::string text = "pid=" + std::to_string(getpid()) + "\nbackend=opened-subscribed\n";
    ssize_t bytes;
    do { bytes = write(fd, text.data(), text.size()); } while (bytes < 0 && errno == EINTR);
    const int saved_errno = bytes < 0 ? errno : EIO;
    const int closed = close(fd);
    if (bytes != static_cast<ssize_t>(text.size())) { errno = saved_errno; Fail("write ready file"); }
    if (closed != 0) Fail("close ready file");
  }
  bool Remove() noexcept {
    if (path_.empty()) return true;
    if (unlink(path_.c_str()) != 0 && errno != ENOENT) { std::perror("cleanup: unlink ready file"); return false; }
    path_.clear();
    return true;
  }
 private:
  std::string path_;
};
void Usage() {
  std::puts("Usage: x5-uvc-mosaic --device /dev/videoN --ready-file PATH\n"
            "Real SC132 CAM1,CAM2 / CAM3,CAM4 2x2 mosaic, MJPEG 1280x960@30.\n"
            "Requires bound 4p_usb/uvc.0 on 35100000.usb, isoc 3072 bytes, no burst.\n"
            "Ready means output opened/subscribed and source startup returned; it is\n"
            "not evidence of host enumeration, acquired frames, or received video.\n"
            "Never stops services or takes over another acquisition owner.");
}
}  // namespace

int main(int argc, char** argv) {
  std::string device, ready_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { Usage(); return 0; }
    if ((arg == "--device" || arg == "--ready-file") && i + 1 < argc) {
      (arg == "--device" ? device : ready_path) = argv[++i];
    } else { Usage(); return 2; }
  }
  if (device.compare(0, 10, "/dev/video") != 0 || device.size() == 10 ||
      device.find_first_not_of("0123456789", 10) != std::string::npos || ready_path.empty()) {
    Usage(); return 2;
  }
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) { std::perror("block termination signals"); return 1; }
  const int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signal_fd < 0) { std::perror("signalfd"); return 1; }
  const int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) { std::perror("eventfd"); close(signal_fd); return 1; }
  Source source(wake_fd);
  Encoder encoder;
  Gadget gadget;
  ReadyFile ready;
  int result = 0;
  try {
    gadget.Open(device);
    source.Start();
    encoder.Open();
    gadget.Activate();
    ready.Create(ready_path);
    std::fprintf(stderr, "ready: UVC output opened/subscribed; source start returned; awaiting real host requests\n");
    bool stopping = false;
    while (!stopping) {
      pollfd fds[] = {{signal_fd, POLLIN, 0}, {wake_fd, POLLIN, 0},
                      {gadget.fd(), static_cast<short>(POLLPRI | (gadget.has_queued() ? POLLOUT : 0)), 0}};
      int count;
      do { count = poll(fds, 3, 100); } while (count < 0 && errno == EINTR);
      if (count < 0) Fail("poll UVC");
      if (fds[0].revents & POLLIN) { stopping = true; continue; }
      if (fds[2].revents & (POLLHUP | POLLNVAL)) throw std::runtime_error("UVC output disappeared");
      // Events have priority over output, especially STREAMOFF/disconnect.
      if (fds[2].revents & POLLPRI) gadget.Events(source);
      if ((fds[2].revents & POLLERR) && gadget.streaming())
        throw std::runtime_error("UVC output queue reported an error");
      if (fds[1].revents & POLLIN) {
        uint64_t count_value;
        if (read(wake_fd, &count_value, sizeof(count_value)) < 0 && errno != EAGAIN && errno != EINTR) Fail("read frame wake");
      }
      source.CheckHealth();
      const int index = gadget.FreeBuffer();
      if (index >= 0) {
        Group group;
        if (source.Take(group)) gadget.Send(index, group, encoder);
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "fatal: %s\n", error.what());
    result = 1;
  } catch (...) {
    std::fprintf(stderr, "fatal: unknown backend failure\n");
    result = 1;
  }
  if (!ready.Remove()) result = 1;
  source.SetStreaming(false);
  const bool source_stopped = source.Stop();
  if (!source_stopped || source.failed()) result = 1;
  if (!gadget.Close()) result = 1;
  if (!encoder.Close()) result = 1;
  if (close(wake_fd) != 0) { std::perror("cleanup: close eventfd"); result = 1; }
  if (close(signal_fd) != 0) { std::perror("cleanup: close signalfd"); result = 1; }
  // A failed producer join retains callback/user-data ownership. Do not unwind
  // C++ objects or vendor library destructors while its worker may still run.
  if (!source_stopped) { std::fflush(nullptr); _Exit(1); }
  return result;
}
