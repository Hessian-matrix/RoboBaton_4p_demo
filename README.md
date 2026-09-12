# X5 SC132 四目相机、IMU 与 UART Demo

英文说明：[README_EN.md](README_EN.md)

![RoboBaton 4P](image/4P_Cam.png)

这是 RoboBaton 4P 的 non-ROS 公开 Demo 仓库，提供 X5 板端运行包、SC132 四目相机 RTSP 示例、ICM-42688 IMU 示例、UART1/UART7 示例、公开头文件和匹配的预编译运行库。

> **最终用户说明以 [4P_doc](https://4p-docs.readthedocs.io/en/latest/index.html) 为准。** 本 README 只保留仓库入口、最小运行方式和支持边界；部署、保存、接线、数据/API 合同和故障排查请直接阅读文档站。

## 包含内容

```text
demo/       可部署到 X5 `/root/demo` 的完整运行包
include/    公开 C 头文件
lib/        与当前 Demo/头文件匹配的预编译库
config/     `sensor_demo` 默认 YAML
src/        Demo 示例源码
scripts/    构建、打包和运行包验证入口
```

用户通常只需要 `demo/`。不要只替换单个 ELF、单个 `.so` 或配置文件。

## 版本与运行包匹配

本仓库、`demo/`、预编译库和公开文档按同一发布组合使用。请先确认 `VERSION`、仓库 tag 和运行包 `manifest.sha256`，不要混用不同版本的源码、头文件、动态库和运行包。

板端可查询程序和运行库版本：

```bash
cd /root/demo
./cam_demo --version
./sensor_demo --version
./imu_reader_demo --version
./serial_port_demo --version
```

这些版本查询不需要初始化相机、IMU 或 UART。完整版本、兼容性和 ABI 说明见 [产品版本与兼容性](https://4p-docs.readthedocs.io/en/latest/product-and-compatibility.html)、[API 参考](https://4p-docs.readthedocs.io/en/latest/api-reference.html) 和 [版本更新记录](https://4p-docs.readthedocs.io/en/latest/changelog.html)。

## Demo 选择

| 程序 | 用途 |
|---|---|
| `sensor_demo` | 四路相机、RTSP 和 IMU 联合运行；可选择 ROS1 bag 或 H.264 MP4 保存 |
| `cam_demo` | 四路相机和 RTSP |
| `imu_reader_demo` | 独立 IMU 读取 |
| `serial_port_demo` | UART1/UART7 串口示例；不适用于 DEBUG_UART |

## 默认运行

默认运行包位于 `demo/`；部署完成后在 X5 上执行：

```bash
cd /root/demo
./sensor_demo
```

只运行相机/RTSP：

```bash
./cam_demo
```

只运行 IMU：

```bash
./imu_reader_demo
```

运行 UART1/UART7 示例：

```bash
./serial_port_demo
```

首次上电、网络、部署前置条件和 `cam-service` 要求见 [首次上电与开机使用](https://4p-docs.readthedocs.io/en/latest/first-boot.html) 和 [快速开始](https://4p-docs.readthedocs.io/en/latest/quick-start.html)。

## 保存数据

`sensor_demo` 默认不自动保存；默认保存格式字段为 MP4，默认路径为 `/root/demo/save_mp4/`。需要启用保存时显式选择一种模式：

```bash
# ROS1 bag v2.0
./sensor_demo --record-bag /data/run.bag

# H.264 MP4 session
./sensor_demo --record-mp4-dir /data/mp4_session
```

ROS1 bag 与 MP4 互斥；MP4 只支持 H.264 和完整四路，frame skip 只适用于 ROS1 bag。完整输出文件、停止顺序、完整性判定、partial/recovery 和离线转换见 [数据保存](https://4p-docs.readthedocs.io/en/latest/save-data-guide.html)。

## 从源码构建

本仓库用于开发机交叉编译，X5 板端只运行生成的运行包。构建前需要匹配的 `./include`、`./lib`、X5 交叉编译包和主机 CMake：

```bash
export TOOLCHAIN_FILE="/path/to/aarch64_x5_host_toolchain.cmake"
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
cmake --build build_x5 -j
```

维护者刷新完整运行包时使用：

```bash
scripts/package_runtime.sh --toolchain-file "$TOOLCHAIN_FILE"
python3 scripts/verify_runtime_package.py demo
```

完整工具链、依赖、构建边界和运行包验证见 [公开 Demo 源码编译](https://4p-docs.readthedocs.io/en/latest/open-source-build.html)。

## 部署

板端目标目录为 `/root/demo`。部署必须遵循：

```text
唯一临时目录
→ 完整 manifest 校验
→ 检查旧应用退出
→ 备份旧目录
→ 原子切换
→ help/smoke 验证
→ 失败恢复最近备份
```

不要使用“先删除 `/root/demo`，再上传新包”的方式，也不要在 `cam-service` 停止时进行相机测试。完整命令见 [部署、升级与回滚](https://4p-docs.readthedocs.io/en/latest/deployment-and-upgrade.html)。

## 支持边界摘要

- 相机输出为 NV12 `1280x1088`；RTSP 默认 H.264、H.265 可选，path 为 `/PRR`，四路默认端口为 `554..557`。
- 相机公开支持 `25/30/40/50/60fps`，默认 `30fps`；`rotate=180` 只支持 `30fps`。
- IMU 支持 `25/50/100/200/500/1000/2000Hz`，默认 `1000Hz`，使用 sensor-timestamp FIFO；当前不提供TF 或标定。
- DEBUG_UART 为 `1.8V`；UART1/UART7 为 `3.3V`。UART1/UART7 的 `3V3` 支持输入/输出和外设供电，两个接口共享合计 `500mA` 限制并支持热插拔；硬件通信已通过 V1 验收。
- 相机运行依赖 X5 板端 `cam-service`；本仓库不是通用主机运行包。

完整相机、IMU、UART、时间戳、帧率和数据语义见 [数据合同](https://4p-docs.readthedocs.io/en/latest/data-contracts.html)、[non-ROS Demo 使用](https://4p-docs.readthedocs.io/en/latest/non-ros-demo.html) 和 [硬件连接与安全](https://4p-docs.readthedocs.io/en/latest/hardware-and-safety.html)。

## 故障排查

遇到启动、动态库、RTSP、IMU 或 UART 问题时，请保留 `VERSION`、manifest 校验结果、执行命令、退出码和必要日志，不要提交真实 IP、凭据或内部路径。排查入口：[故障排查](https://4p-docs.readthedocs.io/en/latest/troubleshooting.html)。

## 公开文档索引

- [产品介绍](https://4p-docs.readthedocs.io/en/latest/Product_Introduction.html)
- [产品版本与兼容性](https://4p-docs.readthedocs.io/en/latest/product-and-compatibility.html)
- [首次上电与开机使用](https://4p-docs.readthedocs.io/en/latest/first-boot.html)
- [快速开始](https://4p-docs.readthedocs.io/en/latest/quick-start.html)
- [non-ROS Demo 使用](https://4p-docs.readthedocs.io/en/latest/non-ros-demo.html)
- [部署、升级与回滚](https://4p-docs.readthedocs.io/en/latest/deployment-and-upgrade.html)
- [公开 Demo 源码编译](https://4p-docs.readthedocs.io/en/latest/open-source-build.html)
- [数据保存](https://4p-docs.readthedocs.io/en/latest/save-data-guide.html)
- [数据合同](https://4p-docs.readthedocs.io/en/latest/data-contracts.html)
- [API 参考](https://4p-docs.readthedocs.io/en/latest/api-reference.html)
- [硬件连接与安全](https://4p-docs.readthedocs.io/en/latest/hardware-and-safety.html)
- [故障排查](https://4p-docs.readthedocs.io/en/latest/troubleshooting.html)
- [版本更新记录](https://4p-docs.readthedocs.io/en/latest/changelog.html)

许可证和第三方组件说明以本仓库的 `LICENSE` 及发布说明为准。
