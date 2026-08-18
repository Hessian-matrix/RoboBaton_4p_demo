#pragma once

#include "cam_demo_common.h"

#include <string>

namespace robobaton_demo {

struct SensorDemoYamlConfigState {
  bool imu_print_rate_was_set = false;
  bool save_data_enabled = false;
  std::string save_data_format = "rosbag";
  std::string save_data_path = "/root/save_demo/record.bag";
};

// sensor_demo 运行包内的用户 YAML 配置相对路径。
inline const char* SensorDemoYamlConfigRelativePath() noexcept {
  return "config/sensor_config.yaml";
}

// 按 DEMO_DIR 解析 sensor_demo YAML 配置路径；未设置 DEMO_DIR 时使用当前目录。
std::string SensorDemoYamlConfigPath();

// 读取或创建 sensor_demo YAML 配置，并把受支持的前缀字段写入 options。
void LoadSensorDemoYamlConfig(Options* options, SensorDemoYamlConfigState* state);

// 按已加载的 YAML 默认值解析 sensor_demo CLI；cam_demo 不链接 YAML 实现。
Options ParseSensorDemoCommandLineWithConfig(int argc, char** argv, Options options,
                                             SensorDemoYamlConfigState state);

}  // namespace robobaton_demo
