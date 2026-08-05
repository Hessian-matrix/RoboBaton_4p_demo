#include "sensor_demo_yaml_config.h"
#include "cam_demo_config.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <sys/stat.h>

namespace robobaton_demo {
namespace {

constexpr const char* kDemoDirEnv = "DEMO_DIR";

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return text;
}

std::string Trim(std::string text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
    return std::isspace(value) != 0;
  });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
    return std::isspace(value) != 0;
  }).base();
  if (first >= last) {
    return std::string();
  }
  return std::string(first, last);
}

std::string NormalizeConfigKey(std::string key) {
  key = ToLower(Trim(std::move(key)));
  std::replace(key.begin(), key.end(), '-', '_');
  return key;
}

std::string StripYamlComment(const std::string& line) {
  char quote = '\0';
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char character = line[index];
    if (quote != '\0') {
      if (character == quote) {
        quote = '\0';
      }
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
      continue;
    }
    if (character == '#') {
      return line.substr(0U, index);
    }
  }
  return line;
}

std::string ParseYamlScalar(std::string value) {
  value = Trim(std::move(value));
  if (value.size() >= 2U &&
      ((value.front() == '\'' && value.back() == '\'') ||
       (value.front() == '"' && value.back() == '"'))) {
    return value.substr(1U, value.size() - 2U);
  }
  return value;
}

std::size_t CountLeadingSpaces(const std::string& text) noexcept {
  std::size_t count = 0U;
  while (count < text.size() && (text[count] == ' ' || text[count] == '\t')) {
    ++count;
  }
  return count;
}

bool IsSupportedSection(const std::string& section) noexcept {
  return section == "camera" || section == "rtsp" || section == "imu" ||
         section == "save_data";
}

int ParseInt(const std::string& text, const char* name) {
  size_t parsed = 0;
  const int value = std::stoi(text, &parsed);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid integer for ") + name);
  }
  return value;
}

uint32_t ParseUint32(const std::string& text, const char* name) {
  if (!text.empty() && text.front() == '-') {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  size_t parsed = 0;
  const unsigned long value = std::stoul(text, &parsed, 0);
  if (parsed != text.size() || value > 0xffffffffUL) {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  return static_cast<uint32_t>(value);
}

long long ParseLongLong(const std::string& text, const char* name) {
  size_t parsed = 0;
  const long long value = std::stoll(text, &parsed);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid integer for ") + name);
  }
  return value;
}

bool ParseBool(const std::string& text, const char* name) {
  const std::string normalized = ToLower(text);
  if (normalized == "true" || normalized == "on" || normalized == "yes" ||
      normalized == "1") {
    return true;
  }
  if (normalized == "false" || normalized == "off" || normalized == "no" ||
      normalized == "0") {
    return false;
  }
  throw std::invalid_argument(std::string("invalid boolean for ") + name);
}

VideoCodec ParseVideoCodec(const std::string& text) {
  if (text == "h264") {
    return VideoCodec::kH264;
  }
  if (text == "h265") {
    return VideoCodec::kH265;
  }
  throw std::invalid_argument("sensor_config.rtsp.codec must be h264 or h265");
}

// YAML 只暴露当前固定分辨率合同；非默认宽高在配置边界直接拒绝。
void RequireFixedCameraDimension(int value, int expected, const char* name) {
  if (value != expected) {
    throw std::invalid_argument(std::string(name) + " must remain " +
                                std::to_string(expected) +
                                "; changing resolution is not supported");
  }
}

bool PathExists(const std::string& path) {
  struct stat status {};
  if (stat(path.c_str(), &status) == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return false;
  }
  throw std::runtime_error("stat sensor_demo config failed: " + path + ": " +
                           std::strerror(errno));
}

bool HasHelpOption(int argc, char** argv) noexcept {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      return true;
    }
  }
  return false;
}

void EnsureParentDirectory(const std::string& path) {
  const std::size_t separator = path.find_last_of('/');
  if (separator == std::string::npos || separator == 0U) {
    return;
  }
  const std::string parent = path.substr(0U, separator);
  if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
    throw std::runtime_error("create sensor_demo config directory failed: " + parent +
                             ": " + std::strerror(errno));
  }
}

std::string DefaultSensorDemoYamlConfigText() {
  const Options defaults;
  std::ostringstream output;
  output << "# sensor_demo YAML config. Command-line options override this file.\n"
         << "# width/height are fixed at 1280x1088; changing them is not supported.\n"
         << "camera:\n"
         << "  width: " << defaults.width << "\n"
         << "  height: " << defaults.height << "\n"
         << "  fps: " << defaults.fps << "\n"
         << "  rotate: " << defaults.rotate_degrees << "\n"
         << "rtsp:\n"
         << "  bps: " << defaults.bps << "\n"
         << "  codec: " << VideoCodecName(defaults.video_codec) << "\n"
         << "  url: " << defaults.url << "\n"
         << "imu:\n"
         << "  sample_rate_hz: " << defaults.imu_sample_rate_hz << "\n"
         << "  print_rate_hz: " << kDefaultImuPrintRateHz << "\n"
         << "  print_metrics: false\n"
         << "save_data:\n"
         << "  save: false\n"
         << "  save_path: " << SensorDemoYamlConfigState{}.save_data_path << "\n"
         << "  skip: false\n";
  return output.str();
}

void EnsureSensorDemoYamlConfigFile(const std::string& path) {
  if (PathExists(path)) {
    return;
  }
  EnsureParentDirectory(path);
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("create sensor_demo config failed: " + path);
  }
  output << DefaultSensorDemoYamlConfigText();
}

bool ApplyYamlConfigValue(const std::string& key, const std::string& value,
                          Options* options, SensorDemoYamlConfigState* state) {
  const std::string name = "sensor_config." + key;
  if (key == "camera.width") {
    const int width = ParseInt(value, name.c_str());
    RequireFixedCameraDimension(width, kDefaultWidth, "camera.width");
    options->width = width;
  } else if (key == "camera.height") {
    const int height = ParseInt(value, name.c_str());
    RequireFixedCameraDimension(height, kDefaultHeight, "camera.height");
    options->height = height;
  } else if (key == "camera.fps") {
    options->fps = ParseInt(value, name.c_str());
  } else if (key == "camera.rotate") {
    options->rotate_degrees = ParseInt(value, name.c_str());
  } else if (key == "rtsp.bps") {
    options->bps = ParseLongLong(value, name.c_str());
  } else if (key == "rtsp.codec") {
    options->video_codec = ParseVideoCodec(value);
  } else if (key == "rtsp.url") {
    options->url = value;
  } else if (key == "imu.sample_rate_hz") {
    options->imu_sample_rate_hz = ParseUint32(value, name.c_str());
  } else if (key == "imu.print_rate_hz") {
    options->imu_print_rate_hz = ParseUint32(value, name.c_str());
    state->imu_print_rate_was_set = true;
  } else if (key == "imu.print_metrics") {
    options->imu_print_metrics = ParseBool(value, name.c_str());
  } else if (key == "save_data.save") {
    state->save_data_enabled = ParseBool(value, name.c_str());
    options->record_bag_path =
        state->save_data_enabled ? state->save_data_path : std::string();
  } else if (key == "save_data.save_path") {
    if (value.empty() || value.front() != '/') {
      throw std::invalid_argument("save_data.save_path must be an absolute path");
    }
    state->save_data_path = value;
    if (state->save_data_enabled) {
      options->record_bag_path = state->save_data_path;
    }
  } else if (key == "save_data.skip") {
    options->record_frame_skip = ParseBool(value, name.c_str()) ? 1U : 0U;
  } else {
    return false;
  }
  return true;
}

void LoadSensorDemoYamlConfigFile(const std::string& path, Options* options,
                                  SensorDemoYamlConfigState* state) {
  EnsureSensorDemoYamlConfigFile(path);
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("open sensor_demo config failed: " + path);
  }

  std::string current_section;
  std::string line;
  std::size_t line_number = 0U;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string without_comment = StripYamlComment(line);
    const std::string stripped = Trim(without_comment);
    if (stripped.empty()) {
      continue;
    }
    const std::size_t separator = stripped.find(':');
    if (separator == std::string::npos) {
      throw std::invalid_argument("invalid sensor config line " + path + ":" +
                                  std::to_string(line_number));
    }

    const std::size_t leading_spaces = CountLeadingSpaces(without_comment);
    const std::string key = NormalizeConfigKey(stripped.substr(0U, separator));
    const std::string value = ParseYamlScalar(stripped.substr(separator + 1U));
    if (key.empty()) {
      throw std::invalid_argument("invalid sensor config entry " + path + ":" +
                                  std::to_string(line_number));
    }

    if (value.empty() && leading_spaces == 0U) {
      if (!IsSupportedSection(key)) {
        throw std::invalid_argument("unknown sensor config section: " + key);
      }
      current_section = key;
      continue;
    }
    if (value.empty()) {
      throw std::invalid_argument("invalid sensor config entry " + path + ":" +
                                  std::to_string(line_number));
    }

    std::string full_key;
    if (leading_spaces > 0U) {
      if (current_section.empty()) {
        throw std::invalid_argument("nested sensor config key without section: " + key);
      }
      full_key = current_section + "." + key;
    } else {
      current_section.clear();
      full_key = key;
    }

    try {
      if (!ApplyYamlConfigValue(full_key, value, options, state)) {
        throw std::invalid_argument("unknown sensor config key: " + full_key);
      }
    } catch (const std::exception& error) {
      throw std::invalid_argument("invalid sensor config " + path + ":" +
                                  std::to_string(line_number) + ": " + error.what());
    }
  }
}

}  // namespace

std::string SensorDemoYamlConfigPath() {
  const char* demo_dir = std::getenv(kDemoDirEnv);
  if (demo_dir != nullptr && demo_dir[0] != '\0') {
    return std::string(demo_dir) + "/" + SensorDemoYamlConfigRelativePath();
  }
  return SensorDemoYamlConfigRelativePath();
}

void LoadSensorDemoYamlConfig(Options* options, SensorDemoYamlConfigState* state) {
  if (options == nullptr || state == nullptr) {
    throw std::invalid_argument("LoadSensorDemoYamlConfig requires non-null arguments");
  }
  LoadSensorDemoYamlConfigFile(SensorDemoYamlConfigPath(), options, state);
}

Options ParseSensorDemoCommandLine(int argc, char** argv) {
  Options options;
  SensorDemoYamlConfigState state;
  if (!HasHelpOption(argc, argv)) {
    LoadSensorDemoYamlConfig(&options, &state);
  }
  return ParseSensorDemoCommandLineWithConfig(argc, argv, std::move(options), state);
}

}  // namespace robobaton_demo
