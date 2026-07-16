#include "cam_demo_config.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace robobaton_demo {
namespace {

constexpr const char* kSc132SensorProfileEnv = "SC132_SENSOR_PROFILE";
constexpr const char* kSc132TriggerModeEnv = "SC132_TRIGGER_MODE";
constexpr const char* kSc132Single60FpsProfile =
    "sc132gs_linear_1088x1280_raw10_60fps_1lane";

// 功能：打印 cam_demo 支持的命令行参数。
// 输入：program 为可执行文件名。
// 输出：帮助文本写入 stdout。
void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --width <pixels>  Frame width, default " << kDefaultWidth << "\n"
            << "  --height <pixels> Frame height, default " << kDefaultHeight << "\n"
            << "  --fps <30|60>     Camera and encoder fps, default 60\n"
            << "  --rotate <0|90|180|270> Output rotation, default 0; 180 is limited to 30fps\n"
            << "  --bps <kbps>      Encoder bitrate in kbps, default " << kDefaultBps << "\n"
            << "  --url <path>      RTSP URL path, default /PRR\n"
            << "  --diagnostics     Print per-channel RTSP timing diagnostics\n"
            << "  --diag-interval-ms <ms> Diagnostics interval, default 1000\n"
            << "  --max-skew-ns <ns> Timestamp skew diagnostic threshold, default 1000000\n"
            << "  --frame-timeout-ms <ms> Frame-set pending timeout, default 100\n"
            << "  --trigger-mode <software_gpio|vin_lpwm|none> SC132 trigger output mode, default "
            << kDefaultSc132TriggerMode << "\n"
            << "  -h, --help        Show this help\n";
}

// 功能：读取当前参数后面的取值。
// 输入：argc/argv、当前参数下标 index、参数名 name。
// 输出：参数值字符串，同时把 index 前移到值所在位置。
// 异常：缺少值时抛出 std::invalid_argument。
std::string RequireValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

// 功能：解析整型命令行参数。
// 输入：text 为参数文本，name 用于错误提示。
// 输出：int 数值。
// 异常：包含非数字尾缀或超出 stoi 能力时抛出异常。
int ParseInt(const std::string& text, const char* name) {
  size_t parsed = 0;
  const int value = std::stoi(text, &parsed);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid integer for ") + name);
  }
  return value;
}

// 功能：解析无符号 mask，支持 0x 前缀。
// 输入：text 为参数文本，name 用于错误提示。
// 输出：uint32_t 数值。
// 异常：包含非数字尾缀或超出 uint32_t 时抛出异常。
uint32_t ParseUint32(const std::string& text, const char* name) {
  size_t parsed = 0;
  const unsigned long value = std::stoul(text, &parsed, 0);
  if (parsed != text.size() || value > 0xffffffffUL) {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  return static_cast<uint32_t>(value);
}

// 功能：解析长整型命令行参数。
// 输入：text 为参数文本，name 用于错误提示。
// 输出：long long 数值。
// 异常：包含非数字尾缀或超出 stoll 能力时抛出异常。
long long ParseLongLong(const std::string& text, const char* name) {
  size_t parsed = 0;
  const long long value = std::stoll(text, &parsed);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid integer for ") + name);
  }
  return value;
}

// 功能：检查运行参数是否处于 demo 支持范围。
// 输入：已解析的 Options。
// 输出：无。
// 异常：参数不合法时抛出 std::invalid_argument。
void ValidateOptions(const Options& options) {
  // 2026-06-17 修改原因：交付主路径只支持完整四目；内部诊断只支持单颗物理 sensor，继续拒绝未验证的 2/3 路组合。
  if (options.channels != 1 && options.channels != kMaxChannels) {
    throw std::invalid_argument("--channels is an internal debug option and only supports 1 or 4");
  }
  if (!IsSupportedCameraMask(options.camera_mask) ||
      options.channels != CameraMaskPopCount(options.camera_mask)) {
    throw std::invalid_argument("--camera-mask supports only 0x1, 0x2, 0x4, 0x8, or 0xF");
  }
  if (options.width <= 0 || options.height <= 0 ||
      (OutputWidth(options) & 1) != 0 || (OutputHeight(options) & 1) != 0) {
    throw std::invalid_argument("--width and --height must produce positive even NV12 dimensions");
  }
  if (options.fps != 30 && options.fps != 60) {
    throw std::invalid_argument("--fps must be 30 or 60");
  }
  if (options.bps <= 0 ||
      static_cast<unsigned long long>(options.bps) >
          static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
    throw std::invalid_argument("--bps must fit the v2 uint32 bitrate field");
  }
  if (options.url.size() < 2U || options.url.size() > 56U || options.url.front() != '/') {
    throw std::invalid_argument("--url must be a 2..56 byte path starting with '/'");
  }
  // 2026-07-15 修改原因：在任何 SC/vendor side effect 前拒绝 v2 path 禁止字符，
  // 避免 RTSP open 阶段才发现 query/fragment/escape/反斜杠或空白歧义。
  for (unsigned char character : options.url) {
    if (character < 0x21U || character > 0x7eU || character == '?' ||
        character == '#' || character == '%' || character == '\\') {
      throw std::invalid_argument("--url contains a v2-forbidden character");
    }
  }
  if (options.rotate_degrees != 0 && options.rotate_degrees != 90 &&
      options.rotate_degrees != 180 && options.rotate_degrees != 270) {
    throw std::invalid_argument("--rotate must be 0, 90, 180, or 270");
  }
  // 2026-06-17 修改原因：对外 rotate=0 表示正装画面；实际底层 rotate=270 的慢路径对应对外 rotate=180，四路 60fps 下不支持。
  if (InternalRotateDegrees(options) == 270 && options.fps == 60) {
    throw std::invalid_argument("--rotate 180 is not supported at 60fps; use --fps 30 or --rotate 0");
  }
  if (options.diagnostic_interval_ms < 100) {
    throw std::invalid_argument("--diag-interval-ms must be >= 100");
  }
  if (options.frame_set_max_skew_ns == 0) {
    throw std::invalid_argument("--max-skew-ns must be positive");
  }
  if (options.frame_set_timeout_ms == 0) {
    throw std::invalid_argument("--frame-timeout-ms must be positive");
  }
  if (options.trigger_mode != "software_gpio" && options.trigger_mode != "gpio" &&
      options.trigger_mode != "vin_lpwm" && options.trigger_mode != "lpwm" &&
      options.trigger_mode != "none" && options.trigger_mode != "off") {
    throw std::invalid_argument("--trigger-mode must be one of software_gpio, vin_lpwm, or none");
  }
}

}  // namespace

// 功能：解析 cam_demo 命令行。
// 输入：main 函数传入的 argc/argv。
// 输出：Options；--help 会打印帮助并退出进程。
// 异常：未知参数或参数值非法时抛出 std::invalid_argument。
Options ParseCommandLine(int argc, char** argv) {
  Options options;
  int requested_channels = options.channels;
  bool channels_set = false;
  bool camera_selector_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--channels") {
      options.channels = ParseInt(RequireValue(argc, argv, &i, "--channels"), "--channels");
      requested_channels = options.channels;
      channels_set = true;
      if (!camera_selector_set) {
        options.camera_mask = CameraMaskFromChannelCount(options.channels);
      }
    } else if (arg == "--camera-id") {
      const int camera_id = ParseInt(RequireValue(argc, argv, &i, "--camera-id"), "--camera-id");
      if (camera_id < 0 || camera_id >= kMaxChannels) {
        throw std::invalid_argument("--camera-id must be 0, 1, 2, or 3");
      }
      // 2026-06-17 修改原因：内部单颗 sensor 诊断按物理 id 选路，避免继续使用“前 N 路”语义误导接线排查。
      options.camera_mask = 1U << static_cast<uint32_t>(camera_id);
      options.channels = 1;
      camera_selector_set = true;
    } else if (arg == "--camera-mask") {
      const uint32_t camera_mask = ParseUint32(RequireValue(argc, argv, &i, "--camera-mask"),
                                              "--camera-mask");
      // 2026-06-17 修改原因：仅允许单颗或四颗，防止把未验证的 2/3 路 SDK 组合伪装成 mask 路径。
      if (!IsSupportedCameraMask(camera_mask)) {
        throw std::invalid_argument("--camera-mask supports only 0x1, 0x2, 0x4, 0x8, or 0xF");
      }
      options.camera_mask = camera_mask;
      options.channels = CameraMaskPopCount(camera_mask);
      camera_selector_set = true;
    } else if (arg == "--width") {
      options.width = ParseInt(RequireValue(argc, argv, &i, "--width"), "--width");
    } else if (arg == "--height") {
      options.height = ParseInt(RequireValue(argc, argv, &i, "--height"), "--height");
    } else if (arg == "--fps") {
      options.fps = ParseInt(RequireValue(argc, argv, &i, "--fps"), "--fps");
    } else if (arg == "--bps") {
      options.bps = ParseLongLong(RequireValue(argc, argv, &i, "--bps"), "--bps");
    } else if (arg == "--url") {
      options.url = RequireValue(argc, argv, &i, "--url");
    } else if (arg == "--rotate") {
      options.rotate_degrees = ParseInt(RequireValue(argc, argv, &i, "--rotate"), "--rotate");
    } else if (arg == "--diagnostics") {
      options.diagnostics = true;
    } else if (arg == "--diag-interval-ms") {
      options.diagnostic_interval_ms =
          ParseInt(RequireValue(argc, argv, &i, "--diag-interval-ms"), "--diag-interval-ms");
    } else if (arg == "--max-skew-ns") {
      options.frame_set_max_skew_ns = static_cast<uint64_t>(
          ParseLongLong(RequireValue(argc, argv, &i, "--max-skew-ns"), "--max-skew-ns"));
    } else if (arg == "--frame-timeout-ms") {
      options.frame_set_timeout_ms = static_cast<uint32_t>(
          ParseInt(RequireValue(argc, argv, &i, "--frame-timeout-ms"), "--frame-timeout-ms"));
    } else if (arg == "--trigger-mode") {
      options.trigger_mode = RequireValue(argc, argv, &i, "--trigger-mode");
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }

  if (!channels_set && !camera_selector_set) {
    options.camera_mask = CameraMaskFromChannelCount(options.channels);
  }
  if (channels_set && camera_selector_set &&
      requested_channels != CameraMaskPopCount(options.camera_mask)) {
    throw std::invalid_argument("--channels conflicts with --camera-id/--camera-mask");
  }

  ValidateOptions(options);
  return options;
}

// 功能：把命令行选择的触发模式写入 libsc132 使用的环境变量。
// 输入：options.trigger_mode，支持 software_gpio、vin_lpwm、none 等别名。
// 副作用：覆盖当前进程的 SC132_TRIGGER_MODE；software_gpio 模式使用 GPIO417。
void ConfigureSc132TriggerMode(const Options& options) {
  // 命令行参数优先于 shell 环境，确保本次进程按显式配置启动。
  if (setenv(kSc132TriggerModeEnv, options.trigger_mode.c_str(), 1) != 0) {
    throw std::runtime_error("set SC132_TRIGGER_MODE failed");
  }
  std::cout << kSc132TriggerModeEnv << "=" << options.trigger_mode
            << " (GPIO417 is used when mode=software_gpio)\n";
}

// 功能：为内部单颗 sensor smoke 自动补齐 60fps sensor profile。
// 输入：options.camera_mask/options.fps。
// 副作用：当内部诊断只启用一颗 sensor 且未预设 SC132_SENSOR_PROFILE 时设置兼容 profile。
void ConfigureSc132SensorProfile(const Options& options) {
  const char* current_profile = std::getenv(kSc132SensorProfileEnv);
  if (current_profile != nullptr && current_profile[0] != '\0') {
    std::cout << "SC132 sensor profile already configured\n";
    return;
  }

  // 2026-06-17 修改原因：四路同步路径沿用 libsc132 默认配置；单颗物理 sensor 60fps 需要 1lane profile 才能和 SDK pipeline 匹配。
  if (CameraMaskPopCount(options.camera_mask) != 1 || options.fps != 60) {
    return;
  }

  // 2026-06-17 修改原因：setenv 只影响当前进程，避免修改板端全局 shell 环境。
  if (setenv(kSc132SensorProfileEnv, kSc132Single60FpsProfile, 1) != 0) {
    throw std::runtime_error("set SC132_SENSOR_PROFILE failed");
  }
  std::cout << "Auto selected single-sensor 60fps profile\n";
}

}  // namespace robobaton_demo
