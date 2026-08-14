#include "cam_demo_config.h"
#include "sensor_demo_yaml_config.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace robobaton_demo {
namespace {

constexpr const char* kSc132SensorProfileEnv = "SC132_SENSOR_PROFILE";
constexpr const char* kSc132TriggerModeEnv = "SC132_TRIGGER_MODE";
constexpr const char* kSc132Single60FpsProfile =
    "sc132gs_linear_1088x1280_raw10_60fps_1lane";

struct ParseState {
  bool channels_set = false;
  bool camera_selector_set = false;
  bool record_frame_skip_set = false;
  int requested_channels = kMaxChannels;
};

// 功能：打印 demo 支持的命令行参数。
// 输入：program 为可执行文件名；include_imu_options 表示是否显示 sensor_demo 专属 IMU 参数。
// 输出：帮助文本写入 stdout。
void PrintUsage(const char* program, bool include_imu_options) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --width <pixels>  Frame width, default " << kDefaultWidth << "\n"
            << "  --height <pixels> Frame height, default " << kDefaultHeight << "\n"
            << "  --fps <25|30|40|50|60> Camera and encoder fps, default 30\n"
            << "  --rotate <0|90|180|270> Output rotation, default 0; 180 is supported only at 30fps\n"
            << "  --bps <kbps>      Encoder bitrate in kbps, default " << kDefaultBps << "\n"
            << "  --codec <h264|h265> Encoder format, default h264\n"
            << "  --url <path>      RTSP URL path, default /PRR\n"
            << "  --rtsp-base-port <port> RTSP first channel port, default "
            << kDefaultRtspBasePort << "\n"
            << "  --diagnostics     Print source liveness and per-channel RTSP timing diagnostics\n"
            << "  --diag-interval-ms <ms> Diagnostics interval, default 1000\n"
            << "  --max-skew-ns <ns> Frame-set timestamp skew limit, default "
            << kDefaultFrameSetMaxSkewNs << "\n"
            << "  --frame-timeout-ms <ms> Frame-set pending timeout, default 100\n"
            << "  --trigger-mode <software_gpio|vin_lpwm|none> SC132 trigger output mode, default "
            << kDefaultSc132TriggerMode << "\n";
  if (include_imu_options) {
    std::cout << "  " << SensorDemoYamlConfigRelativePath()
              << " YAML config is loaded before CLI options; missing file is created with defaults\n";
    std::cout << "  --sample-rate-hz <25|50|100|200|500|1000|2000> IMU sample rate, default "
              << kDefaultImuSampleRateHz << "\n";
    std::cout << "  --imu-sample-drop-policy <allow-counted|strict> IMU timing sample-drop policy, default allow-counted\n";
    std::cout << "  --imu-start-order <imu-first|camera-first> IMU startup order, default camera-first\n";
    std::cout << "  --print-rate-hz HZ IMU terminal output rate, default min(sample-rate-hz, 10); 0 disables IMU sample output\n";
    std::cout << "  --print-metrics Include metrics diagnostics section in each IMU output record, default off\n";
    std::cout << "  --record-bag <absolute-path> Write one ROS1 bag while sensor_demo runs\n";
    std::cout << "  --record-frame-skip <0|1> With --record-bag, 0 saves every frame-set, 1 saves alternate frame-sets; default 0\n";
  }
  std::cout << "  -h, --help        Show this help\n";
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

// 功能：解析无符号 64 位 CLI 数值，支持 0x 前缀并拒绝负号。
uint64_t ParseUint64(const std::string& text, const char* name) {
  if (!text.empty() && text.front() == '-') {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  size_t parsed = 0;
  const unsigned long long value = std::stoull(text, &parsed, 0);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  return static_cast<uint64_t>(value);
}

// 将不可信的 CLI 编码格式收敛为强类型枚举，仅接受 h264/h265。
// 输入：小写编码格式文本；输出：VideoCodec；非法值抛出 std::invalid_argument。
VideoCodec ParseVideoCodec(const std::string& text) {
  if (text == "h264") {
    return VideoCodec::kH264;
  }
  if (text == "h265") {
    return VideoCodec::kH265;
  }
  throw std::invalid_argument("--codec must be h264 or h265");
}

// 将 sensor_demo 的策略文本映射到 ABI v2 reserved[0] 允许的公开枚举。
uint32_t ParseImuSampleDropPolicy(const std::string& text) {
  if (text == "allow-counted") {
    return ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED;
  }
  if (text == "strict") {
    return ICM42688_SAMPLE_DROP_POLICY_STRICT;
  }
  throw std::invalid_argument(
      "--imu-sample-drop-policy must be allow-counted or strict");
}

// 将 sensor_demo 的启动顺序文本收敛为内部强类型枚举。
ImuStartOrder ParseImuStartOrder(const std::string& text) {
  if (text == "imu-first") {
    return ImuStartOrder::kImuFirst;
  }
  if (text == "camera-first") {
    return ImuStartOrder::kCameraFirst;
  }
  throw std::invalid_argument("--imu-start-order must be imu-first or camera-first");
}




void ApplyChannels(Options* options, ParseState* state, int channels) {
  options->channels = channels;
  state->requested_channels = channels;
  state->channels_set = true;
  if (!state->camera_selector_set) {
    options->camera_mask = CameraMaskFromChannelCount(options->channels);
  }
}

void ApplyCameraId(Options* options, ParseState* state, int camera_id) {
  if (camera_id < 0 || camera_id >= kMaxChannels) {
    throw std::invalid_argument("--camera-id must be 0, 1, 2, or 3");
  }
  // 单颗诊断按物理 camera id 选路。
  options->camera_mask = 1U << static_cast<uint32_t>(camera_id);
  options->channels = 1;
  state->camera_selector_set = true;
}

void ApplyCameraMask(Options* options, ParseState* state, uint32_t camera_mask) {
  // camera mask 仅接受单颗诊断或完整四目组合。
  if (!IsSupportedCameraMask(camera_mask)) {
    throw std::invalid_argument("--camera-mask supports only 0x1, 0x2, 0x4, 0x8, or 0xF");
  }
  options->camera_mask = camera_mask;
  options->channels = CameraMaskPopCount(camera_mask);
  state->camera_selector_set = true;
}


void ValidateOptions(const Options& options, bool record_frame_skip_set);

void ValidateSelectorState(const ParseState& state, const Options& options) {
  if (state.channels_set && state.camera_selector_set &&
      state.requested_channels != CameraMaskPopCount(options.camera_mask)) {
    throw std::invalid_argument("--channels conflicts with --camera-id/--camera-mask");
  }
}

void FinalizeParsedOptions(Options* options, const ParseState& config_state,
                           const ParseState& cli_state,
                           const SensorDemoYamlConfigState& sensor_config_state,
                           bool accept_imu_options) {
  const bool cli_selects_camera = cli_state.channels_set || cli_state.camera_selector_set;
  const ParseState& selector_state = cli_selects_camera ? cli_state : config_state;
  if (!selector_state.channels_set && !selector_state.camera_selector_set) {
    options->camera_mask = CameraMaskFromChannelCount(options->channels);
  }
  ValidateSelectorState(selector_state, *options);

  if (accept_imu_options && !sensor_config_state.imu_print_rate_was_set) {
    options->imu_print_rate_hz =
        std::min(options->imu_sample_rate_hz, kDefaultImuPrintRateHz);
  }

  ValidateOptions(*options, cli_state.record_frame_skip_set);
}

// 功能：按 libicm42688 C ABI 当前公开的离散 ODR 表校验 IMU 采样率。
// 输入：sample_rate_hz 为用户命令行值。
// 输出：支持则 true，否则 false。
bool IsSupportedImuSampleRateHz(uint32_t sample_rate_hz) {
  switch (sample_rate_hz) {
    case 25U:
    case 50U:
    case 100U:
    case 200U:
    case 500U:
    case 1000U:
    case 2000U:
      return true;
    default:
      return false;
  }
}

// 功能：按 libsc132 当前公开的离散帧率表校验相机帧率。
bool IsSupportedCameraFps(int fps) {
  switch (fps) {
    case 25:
    case 30:
    case 40:
    case 50:
    case 60:
      return true;
    default:
      return false;
  }
}


// 功能：检查运行参数是否处于 demo 支持范围。
// 输入：已解析的 Options。
// 输出：无。
// 异常：参数不合法时抛出 std::invalid_argument。
void ValidateOptions(const Options& options, bool record_frame_skip_set) {
  // 交付路径仅支持完整四目，内部诊断仅支持单颗物理 sensor。
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
  if (!IsSupportedCameraFps(options.fps)) {
    throw std::invalid_argument("--fps must be one of 25, 30, 40, 50, or 60");
  }
  if (options.bps <= 0 ||
      static_cast<unsigned long long>(options.bps) >
          static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
    throw std::invalid_argument("--bps must fit the v2 uint32 bitrate field");
  }
  if (options.url.size() < 2U || options.url.size() > 56U || options.url.front() != '/') {
    throw std::invalid_argument("--url must be a 2..56 byte path starting with '/'");
  }
  // 在任何副作用前拒绝 query、fragment、escape 或路径歧义字符。
  for (unsigned char character : options.url) {
    if (character < 0x21U || character > 0x7eU || character == '?' ||
        character == '#' || character == '%' || character == '\\') {
      throw std::invalid_argument("--url contains a v2-forbidden character");
    }
  }
  if (options.rtsp_base_port <= 0 ||
      options.rtsp_base_port > kMaxRtspPort - (kMaxChannels - 1)) {
    throw std::invalid_argument("--rtsp-base-port must keep four channel ports in 1..65535");
  }
  if (options.rotate_degrees != 0 && options.rotate_degrees != 90 &&
      options.rotate_degrees != 180 && options.rotate_degrees != 270) {
    throw std::invalid_argument("--rotate must be 0, 90, 180, or 270");
  }
  // 对外 180 度进入底层 270 度慢路径，只保留 30fps 作为已验证组合。
  if (InternalRotateDegrees(options) == 270 && options.fps != 30) {
    throw std::invalid_argument("--rotate 180 is supported only at 30fps");
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
  if (!IsSupportedImuSampleRateHz(options.imu_sample_rate_hz)) {
    throw std::invalid_argument(
        "--sample-rate-hz must be one of 25, 50, 100, 200, 500, 1000, or 2000");
  }
  if (options.record_frame_skip > 1U) {
    throw std::invalid_argument("--record-frame-skip must be 0 or 1");
  }
  if (!options.record_bag_path.empty() && options.record_bag_path.front() != '/') {
    throw std::invalid_argument("--record-bag/save_data.save_path path must be absolute");
  }
  if (record_frame_skip_set && options.record_bag_path.empty()) {
    throw std::invalid_argument("--record-frame-skip requires --record-bag");
  }
  if (options.imu_print_rate_hz > options.imu_sample_rate_hz) {
    throw std::invalid_argument("--print-rate-hz must not exceed --sample-rate-hz");
  }
  if (options.imu_sample_drop_policy > ICM42688_SAMPLE_DROP_POLICY_STRICT) {
    throw std::invalid_argument(
        "--imu-sample-drop-policy must be allow-counted or strict");
  }
  if (options.trigger_mode != "software_gpio" && options.trigger_mode != "gpio" &&
      options.trigger_mode != "vin_lpwm" && options.trigger_mode != "lpwm" &&
      options.trigger_mode != "none" && options.trigger_mode != "off") {
    throw std::invalid_argument("--trigger-mode must be one of software_gpio, vin_lpwm, or none");
  }
}

}  // namespace

// 功能：解析相机/RTSP命令行；sensor_demo 参数路径可接受已加载的 YAML 默认值。
// 输入：main 函数传入的 argc/argv；accept_imu_options 控制 sensor_demo 专属参数。
// 输出：Options；--help 会打印帮助并退出进程。
// 异常：未知参数或参数值非法时抛出 std::invalid_argument。
Options ParseCommandLineImpl(int argc, char** argv, bool accept_imu_options,
                             Options options,
                             SensorDemoYamlConfigState sensor_config_state) {
  ParseState config_parse_state;
  config_parse_state.requested_channels = options.channels;

  ParseState cli_parse_state;
  cli_parse_state.requested_channels = options.channels;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--channels") {
      ApplyChannels(&options, &cli_parse_state,
                    ParseInt(RequireValue(argc, argv, &i, "--channels"), "--channels"));
    } else if (arg == "--camera-id") {
      ApplyCameraId(&options, &cli_parse_state,
                    ParseInt(RequireValue(argc, argv, &i, "--camera-id"), "--camera-id"));
    } else if (arg == "--camera-mask") {
      ApplyCameraMask(&options, &cli_parse_state,
                      ParseUint32(RequireValue(argc, argv, &i, "--camera-mask"),
                                  "--camera-mask"));
    } else if (arg == "--width") {
      options.width = ParseInt(RequireValue(argc, argv, &i, "--width"), "--width");
    } else if (arg == "--height") {
      options.height = ParseInt(RequireValue(argc, argv, &i, "--height"), "--height");
    } else if (arg == "--fps") {
      options.fps = ParseInt(RequireValue(argc, argv, &i, "--fps"), "--fps");
    } else if (arg == "--bps") {
      options.bps = ParseLongLong(RequireValue(argc, argv, &i, "--bps"), "--bps");
    } else if (arg == "--codec") {
      options.video_codec = ParseVideoCodec(RequireValue(argc, argv, &i, "--codec"));
    } else if (arg == "--url") {
      options.url = RequireValue(argc, argv, &i, "--url");
    } else if (arg == "--rtsp-base-port") {
      options.rtsp_base_port =
          ParseInt(RequireValue(argc, argv, &i, "--rtsp-base-port"), "--rtsp-base-port");
    } else if (arg == "--rotate") {
      options.rotate_degrees = ParseInt(RequireValue(argc, argv, &i, "--rotate"), "--rotate");
    } else if (arg == "--diagnostics") {
      options.diagnostics = true;
    } else if (arg == "--diag-interval-ms") {
      options.diagnostic_interval_ms =
          ParseInt(RequireValue(argc, argv, &i, "--diag-interval-ms"), "--diag-interval-ms");
    } else if (arg == "--max-skew-ns") {
      options.frame_set_max_skew_ns =
          ParseUint64(RequireValue(argc, argv, &i, "--max-skew-ns"), "--max-skew-ns");
    } else if (arg == "--frame-timeout-ms") {
      options.frame_set_timeout_ms = static_cast<uint32_t>(
          ParseUint32(RequireValue(argc, argv, &i, "--frame-timeout-ms"),
                      "--frame-timeout-ms"));
    } else if (arg == "--trigger-mode") {
      options.trigger_mode = RequireValue(argc, argv, &i, "--trigger-mode");
    } else if (accept_imu_options && arg == "--sample-rate-hz") {
      options.imu_sample_rate_hz =
          ParseUint32(RequireValue(argc, argv, &i, "--sample-rate-hz"), "--sample-rate-hz");
    } else if (accept_imu_options && arg == "--imu-sample-drop-policy") {
      options.imu_sample_drop_policy =
          ParseImuSampleDropPolicy(RequireValue(argc, argv, &i, "--imu-sample-drop-policy"));
    } else if (accept_imu_options && arg == "--imu-start-order") {
      options.imu_start_order =
          ParseImuStartOrder(RequireValue(argc, argv, &i, "--imu-start-order"));
    } else if (accept_imu_options && arg == "--print-rate-hz") {
      options.imu_print_rate_hz =
          ParseUint32(RequireValue(argc, argv, &i, "--print-rate-hz"),
                      "--print-rate-hz");
      sensor_config_state.imu_print_rate_was_set = true;
    } else if (accept_imu_options && arg == "--print-metrics") {
      options.imu_print_metrics = true;
    } else if (accept_imu_options && arg == "--record-bag") {
      options.record_bag_path = RequireValue(argc, argv, &i, "--record-bag");
      if (options.record_bag_path.empty() || options.record_bag_path.front() != '/') {
        throw std::invalid_argument("--record-bag path must be absolute");
      }
    } else if (accept_imu_options && arg == "--record-frame-skip") {
      options.record_frame_skip =
          ParseUint32(RequireValue(argc, argv, &i, "--record-frame-skip"),
                      "--record-frame-skip");
      cli_parse_state.record_frame_skip_set = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0], accept_imu_options);
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }

  FinalizeParsedOptions(&options, config_parse_state, cli_parse_state,
                        sensor_config_state, accept_imu_options);
  return options;
}

Options ParseCommandLine(int argc, char** argv) {
  return ParseCommandLineImpl(argc, argv, false, Options{}, SensorDemoYamlConfigState{});
}

Options ParseSensorDemoCommandLineWithConfig(int argc, char** argv, Options options,
                                             SensorDemoYamlConfigState sensor_config_state) {
  return ParseCommandLineImpl(argc, argv, true, std::move(options), sensor_config_state);
}

// 功能：把命令行选择的触发模式写入 libsc132 使用的环境变量。
// 输入：options.trigger_mode，支持 software_gpio、vin_lpwm、none 等别名。
// 副作用：覆盖当前进程的 SC132_TRIGGER_MODE；software_gpio 模式使用 GPIO417。
void ConfigureSc132TriggerMode(const Options& options) {
  // 命令行参数优先于 shell 环境。
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

  // 四路使用默认 profile；单颗 60fps 使用匹配 SDK 的 1-lane profile。
  if (CameraMaskPopCount(options.camera_mask) != 1 || options.fps != 60) {
    return;
  }

  // setenv 仅影响当前进程，不修改板端全局 shell 环境。
  if (setenv(kSc132SensorProfileEnv, kSc132Single60FpsProfile, 1) != 0) {
    throw std::runtime_error("set SC132_SENSOR_PROFILE failed");
  }
  std::cout << "Auto selected single-sensor 60fps profile\n";
}

}  // namespace robobaton_demo
