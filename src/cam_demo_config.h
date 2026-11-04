#pragma once

#include "cam_demo_common.h"

namespace robobaton_demo {

// 功能：解析命令行参数并做合法性检查。
// 输入：main 函数收到的 argc/argv。
// 输出：完整 Options；参数非法时抛出 std::invalid_argument。
Options ParseCommandLine(int argc, char** argv);

// 功能：解析 sensor_demo 命令行参数，优先读取 DEMO_DIR/config/sensor_config.yaml，再用 CLI 覆盖。
// 输入：main 函数收到的 argc/argv。
// 输出：完整 Options；参数非法时抛出 std::invalid_argument。
Options ParseSensorDemoCommandLine(int argc, char** argv);

// Configure startup-only trigger ownership and camera timestamp selection.
// Sets SC132_TRIGGER_MODE and SC132_TIMESTAMP_PATH before libsc132 starts.
void ConfigureSc132TriggerMode(const Options& options);

// 功能：为特定启动组合选择兼容的 sensor profile。
// 输入：运行参数 options。
// 副作用：必要时设置进程内环境变量 SC132_SENSOR_PROFILE。
void ConfigureSc132SensorProfile(const Options& options);

}  // namespace robobaton_demo
