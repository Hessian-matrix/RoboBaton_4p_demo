#include "cam_demo_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string>
#include <stdexcept>
#include <utility>

extern "C" {
#include "sc132camera.h"
}

namespace robobaton_demo {
namespace {

constexpr uint32_t kAbsentCalibrationBus = 0xffffffffU;
constexpr const char* kCalibrationDirectoryEnv = "SC132_CALIBRATION_DIR";
constexpr const char* kDeviceTreeRootEnv = "SC132_DEVICE_TREE_ROOT";
constexpr const char* kDemoDirEnv = "DEMO_DIR";

const char* NonEmptyEnv(const char* name) noexcept {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::string DefaultCalibrationDirectory() {
  const char* demo_dir = NonEmptyEnv(kDemoDirEnv);
  return std::string(demo_dir != nullptr ? demo_dir : ".") +
         "/config/camera_calibration";
}

void InitializeCalibrationBinding(uint32_t camera_id,
                                  camera_calibration_binding_v1* binding) {
  std::memset(binding, 0, sizeof(*binding));
  binding->struct_size = static_cast<uint32_t>(sizeof(*binding));
  binding->camera_id = camera_id;
  binding->status = CAMERA_CALIBRATION_STATUS_MISSING;
  binding->source = CAMERA_CALIBRATION_SOURCE_NONE;
  binding->bus = kAbsentCalibrationBus;
}

const char* CameraCalibrationSourceName(uint32_t source) noexcept {
  switch (source) {
    case CAMERA_CALIBRATION_SOURCE_EEPROM:
      return "eeprom";
    case CAMERA_CALIBRATION_SOURCE_FILE:
      return "file";
    case CAMERA_CALIBRATION_SOURCE_NONE:
      return "none";
    default:
      return "none";
  }
}

const char* CameraCalibrationStatusName(uint32_t status) noexcept {
  switch (status) {
    case CAMERA_CALIBRATION_STATUS_PASS:
      return "PASS";
    case CAMERA_CALIBRATION_STATUS_ABSENT:
      return "ABSENT";
    case CAMERA_CALIBRATION_STATUS_INVALID:
      return "INVALID";
    case CAMERA_CALIBRATION_STATUS_IO_ERROR:
      return "IO_ERROR";
    case CAMERA_CALIBRATION_STATUS_MISSING:
      return "MISSING";
    default:
      return "IO_ERROR";
  }
}

std::string HexByte(uint32_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text = "0x00";
  text[2] = digits[(value >> 4U) & 0x0fU];
  text[3] = digits[value & 0x0fU];
  return text;
}

}  // namespace


std::atomic<bool> g_stop_requested{false};

QueuedFrame::~QueuedFrame() { Reset(); }

QueuedFrame::QueuedFrame(QueuedFrame&& other) noexcept { *this = std::move(other); }

QueuedFrame& QueuedFrame::operator=(QueuedFrame&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  frame = std::exchange(other.frame, nullptr);
  channel = other.channel;
  sequence = other.sequence;
  frame_id = other.frame_id;
  group_id = other.group_id;
  group_timestamp_ns = other.group_timestamp_ns;
  group_max_skew_ns = other.group_max_skew_ns;
  camera_timestamp_ns = other.camera_timestamp_ns;
  rtsp_timestamp_ns = other.rtsp_timestamp_ns;
  group_timestamp_domain = other.group_timestamp_domain;
  camera_timestamp_domain = other.camera_timestamp_domain;
  rtsp_timestamp_domain = other.rtsp_timestamp_domain;
  enqueue_timestamp_ns = other.enqueue_timestamp_ns;
  y_data = other.y_data;
  uv_data = other.uv_data;
  y_phys = other.y_phys;
  uv_phys = other.uv_phys;
  y_size = other.y_size;
  uv_size = other.uv_size;
  width = other.width;
  height = other.height;
  stride = other.stride;
  vstride = other.vstride;
  return *this;
}

void QueuedFrame::Reset() noexcept {
  if (frame != nullptr) {
    // retained frame 由 RAII 唯一管理，所有路径统一经 Reset release。
    sc132_frame_release(frame);
    frame = nullptr;
  }
}

sc132_frame_t* QueuedFrame::ReleaseOwnership() noexcept {
  return std::exchange(frame, nullptr);
}

CameraCalibrationSet LoadCameraCalibrations(uint32_t camera_mask, int rotate_degrees) {
  CameraCalibrationSet set;
  set.camera_mask = camera_mask;

  std::string default_calibration_directory;
  const char* fallback_directory = NonEmptyEnv(kCalibrationDirectoryEnv);
  if (fallback_directory == nullptr) {
    default_calibration_directory = DefaultCalibrationDirectory();
    fallback_directory = default_calibration_directory.c_str();
  }
  const char* device_tree_root = NonEmptyEnv(kDeviceTreeRootEnv);

  for (uint32_t camera_id = 0U; camera_id < CAMERA_CALIBRATION_CAMERA_COUNT;
       ++camera_id) {
    camera_calibration_binding_v1& binding = set.bindings[camera_id];
    InitializeCalibrationBinding(camera_id, &binding);
    if ((camera_mask & (1U << camera_id)) == 0U) {
      continue;
    }

    camera_calibration_request_v1 request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.camera_id = camera_id;
    request.device_tree_root = device_tree_root;
    request.fallback_directory = fallback_directory;
    request.rotate_degrees = rotate_degrees;
    const int32_t rc = camera_calibration_discover(&request, &binding);
    if (rc != CAMERA_CALIBRATION_OK) {
      InitializeCalibrationBinding(camera_id, &binding);
      binding.status = CAMERA_CALIBRATION_STATUS_IO_ERROR;
      std::snprintf(binding.error, sizeof(binding.error),
                    "camera_calibration_discover failed rc=%d",
                    static_cast<int>(rc));
    }
  }
  return set;
}

void PrintCameraCalibrationResults(const CameraCalibrationSet& set,
                                   std::ostream& output) {
  for (uint32_t camera_id = 0U; camera_id < CAMERA_CALIBRATION_CAMERA_COUNT;
       ++camera_id) {
    if ((set.camera_mask & (1U << camera_id)) == 0U) {
      continue;
    }
    const camera_calibration_binding_v1& binding = set.bindings[camera_id];
    output << "CAMERA_CALIBRATION_RESULT camera=" << camera_id
           << " source=" << CameraCalibrationSourceName(binding.source)
           << " status=" << CameraCalibrationStatusName(binding.status);
    if (binding.bus != kAbsentCalibrationBus) {
      output << " bus=" << binding.bus;
    } else if (binding.status == CAMERA_CALIBRATION_STATUS_PASS &&
               binding.source == CAMERA_CALIBRATION_SOURCE_FILE) {
      output << " bus=unavailable";
    }
    if (binding.eeprom_i2c_addr != 0U) {
      output << " eeprom=" << HexByte(binding.eeprom_i2c_addr);
    }
    if (binding.endpoint[0] != '\0') {
      output << " endpoint=" << binding.endpoint;
    }
    if (binding.path[0] != '\0') {
      output << " path=" << binding.path;
    }
    if (binding.error[0] != '\0') {
      output << " error=" << binding.error;
    }
    output << '\n';
  }
}

uint64_t SteadyClockNowNs() {
  uint64_t timestamp_ns = 0U;
  if (FrozenSystemClock::ReadSystemClock(FrozenClockId::kMonotonicRaw,
                                         &timestamp_ns, nullptr) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW read failed");
  }
  return timestamp_ns;
}

int RtspPortForChannel(int channel) { return kBaseRtspPort + channel; }

// 编码格式名称统一用于用户可见的状态和日志输出。
const char* VideoCodecName(VideoCodec codec) noexcept {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264";
    case VideoCodec::kH265:
      return "h265";
  }
  // 非法枚举统一输出 unknown，避免日志路径产生未定义行为。
  return "unknown";
}

const char* ImuSampleDropPolicyName(uint32_t policy) noexcept {
  switch (policy) {
    case ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED:
      return "allow-counted";
    case ICM42688_SAMPLE_DROP_POLICY_STRICT:
      return "strict";
    default:
      return "invalid";
  }
}

const char* TimestampDomainName(TimestampDomain domain) noexcept {
  switch (domain) {
    case TimestampDomain::kMonotonicRaw:
      return "monotonic_raw";
    case TimestampDomain::kSystemRealtime:
      return "system_realtime";
    case TimestampDomain::kSc132Native:
      return "sc132_native";
    case TimestampDomain::kUnknown:
      break;
  }
  return "unknown";
}

bool Sc132TimestampsAreMonotonicRaw(const Options& options) noexcept {
  return options.trigger_mode == "software_gpio" || options.trigger_mode == "gpio";
}

TimestampDomain Sc132OutputTimestampDomain(const Options& options) noexcept {
  if (!Sc132TimestampsAreMonotonicRaw(options)) {
    return TimestampDomain::kSc132Native;
  }
  return options.system_clock != nullptr ? TimestampDomain::kSystemRealtime
                                        : TimestampDomain::kMonotonicRaw;
}

uint32_t CameraMaskFromChannelCount(int channels) {
  if (channels <= 0) {
    return 0U;
  }
  if (channels >= kMaxChannels) {
    return kDefaultCameraMask;
  }
  return (1U << static_cast<uint32_t>(channels)) - 1U;
}

int CameraMaskPopCount(uint32_t camera_mask) {
  int count = 0;
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (CameraMaskContains(camera_mask, camera_id)) {
      ++count;
    }
  }
  return count;
}

bool CameraMaskContains(uint32_t camera_mask, int camera_id) {
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    return false;
  }
  return (camera_mask & (1U << static_cast<uint32_t>(camera_id))) != 0U;
}

bool IsSupportedCameraMask(uint32_t camera_mask) {
  if (camera_mask == kDefaultCameraMask) {
    return true;
  }
  return CameraMaskPopCount(camera_mask) == 1 &&
         (camera_mask & ~kDefaultCameraMask) == 0U;
}

int OutputWidth(const Options& options) {
  // width/height 是默认横屏交付画布；外部 0/180 保持画布轴，90/270 交换宽高。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.height
             : options.width;
}

int OutputHeight(const Options& options) {
  // 安装补偿只影响底层旋转角度，不改变对外交付画布的宽高判定。
  return options.rotate_degrees == 90 || options.rotate_degrees == 270
             ? options.width
             : options.height;
}

int InternalRotateDegrees(const Options& options) {
  return (options.rotate_degrees + kMountRotateDegrees) % 360;
}

}  // namespace robobaton_demo
