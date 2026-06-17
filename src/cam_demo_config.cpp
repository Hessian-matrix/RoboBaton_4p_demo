#include "cam_demo_config.h"

#include <cstdlib>
#include <iostream>
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
            << "  --width <pixels>  Frame width, default 1088\n"
            << "  --height <pixels> Frame height, default 1280\n"
            << "  --fps <30|60>     Camera and encoder fps, default 60\n"
            << "  --rotate <0|90|180|270> Output rotation, default 90; 270 is limited to 30fps\n"
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
  // 2026-06-16 修改原因：交付只暴露固定四目默认链路；--channels 保留为内部调试入口，拒绝未验证的 2/3 路部分启动。
  if (options.channels != 1 && options.channels != kMaxChannels) {
    throw std::invalid_argument("--channels is an internal debug option and only supports 1 or 4");
  }
  if (options.width <= 0 || options.height <= 0) {
    throw std::invalid_argument("--width and --height must be positive");
  }
  if (options.fps != 30 && options.fps != 60) {
    throw std::invalid_argument("--fps must be 30 or 60");
  }
  if (options.bps <= 0) {
    throw std::invalid_argument("--bps must be positive");
  }
  if (options.url.empty() || options.url.front() != '/') {
    throw std::invalid_argument("--url must start with '/'");
  }
  if (options.rotate_degrees != 0 && options.rotate_degrees != 90 &&
      options.rotate_degrees != 180 && options.rotate_degrees != 270) {
    throw std::invalid_argument("--rotate must be 0, 90, 180, or 270");
  }
  // 2026-06-16 修改原因：实测 rotate=270 的 Nano2D 后处理路径在四路 60fps 下吞吐不达标，启动阶段直接拒绝该非支持组合。
  if (options.rotate_degrees == 270 && options.fps == 60) {
    throw std::invalid_argument("--rotate 270 is not supported at 60fps; use --fps 30 or --rotate 90");
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

  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--channels") {
      options.channels = ParseInt(RequireValue(argc, argv, &i, "--channels"), "--channels");
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

// 功能：为内部单路 smoke 自动补齐 60fps sensor profile。
// 输入：options.channels/options.fps。
// 副作用：当内部调试使用 --channels 1 且未预设 SC132_SENSOR_PROFILE 时设置兼容 profile。
void ConfigureSc132SensorProfile(const Options& options) {
  const char* current_profile = std::getenv(kSc132SensorProfileEnv);
  if (current_profile != nullptr && current_profile[0] != '\0') {
    std::cout << kSc132SensorProfileEnv << " already set to " << current_profile << "\n";
    return;
  }

  // 四路同步路径沿用 libsc132 默认配置；这里只处理单路 60fps 兼容场景。
  if (options.channels != 1 || options.fps != 60) {
    return;
  }

  // setenv 只影响当前进程，避免修改板端全局 shell 环境。
  if (setenv(kSc132SensorProfileEnv, kSc132Single60FpsProfile, 1) != 0) {
    throw std::runtime_error("set SC132_SENSOR_PROFILE failed");
  }
  std::cout << "Auto selected " << kSc132SensorProfileEnv << "="
            << kSc132Single60FpsProfile << " for single-channel 60fps\n";
}

}  // namespace robobaton_demo
