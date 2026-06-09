# X5 SC132 4-Camera, IMU And UART Open Source Demo

English version: [README_EN.md](README_EN.md)

这是给用户交付的最小开源 demo。它包含 SC132 四目相机 RTSP 示例、IMU 读取示例、串口通信示例、公开头文件和二进制驱动库，不包含底层驱动实现源码。

## 1. 目录结构

```text
open_source_demo/
├── CMakeLists.txt
├── README.md
├── README_EN.md
├── include/
│   ├── icm42688_driver.h
│   ├── sc132camera.h
│   └── pr_venc.h
├── lib/
│   ├── libicm42688.so
│   ├── libsc132.so
│   └── libprrtsp.so
├── scripts/
│   ├── build_cam_demo.sh
│   ├── build_imu_reader_demo.sh
│   ├── build_serial_port_demo.sh
│   ├── cam_demo_regression.sh
│   └── package_runtime.sh
└── src/
    ├── cam_demo.cpp
    ├── cam_demo_common.h / cam_demo_common.cpp
    ├── cam_demo_config.h / cam_demo_config.cpp
    ├── cam_demo_pipeline.h / cam_demo_pipeline.cpp
    ├── cam_demo_rtsp.h / cam_demo_rtsp.cpp
    ├── imu_reader_demo.cpp
    └── serial_port_demo.cpp
```

`cam_demo.cpp` 保留主流程和用户二次开发入口；配置解析、RTSP 封装、帧队列和后台推流流程分别拆到 `cam_demo_config.*`、`cam_demo_rtsp.*`、`cam_demo_pipeline.*`，便于用户按模块阅读。

## 2. 构建

本 demo 设计为“开发机交叉编译，X5 板端只运行”，不要求也不建议在 X5 板端原生编译。

构建前需要准备：

- X5 aarch64 交叉编译工具链
- CMake
- X5 SDK 提供的 toolchain file

下面命令中的 toolchain file 路径仅为本机示例，用户需要替换成自己环境里的实际路径：

```bash
cd open_source_demo
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake
cmake --build build_x5 -j
```

也可以只编译单个 demo：

```bash
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_cam_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_imu_reader_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_serial_port_demo.sh
```

脚本默认使用 `build_x5/` 作为构建目录；如果需要指定其他目录，可以设置 `BUILD_DIR=/path/to/build_x5`。

本项目验证时使用的工具链文件路径是：

```text
/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
```

生成文件：

- `build_x5/imu_reader_demo`
- `build_x5/serial_port_demo`
- `build_x5/cam_demo`

检查架构：

```bash
file build_x5/imu_reader_demo
file build_x5/serial_port_demo
file build_x5/cam_demo
file lib/libicm42688.so
file lib/libsc132.so
file lib/libprrtsp.so
```

期望输出包含 `ARM aarch64`。

如果没有交叉编译工具链，则不能重新编译 demo，只能使用已经编译好的 `imu_reader_demo`、`serial_port_demo`、`cam_demo` 和 `lib/` 下对应 `.so` 部署到板端运行。

## 3. 部署

建议部署到 X5 的 `/root/demo/`。先在开发机生成板端运行包：

```bash
cd open_source_demo
scripts/package_runtime.sh --build-dir build_x5 --output-dir deploy_runtime
```

`package_runtime.sh` 会生成顶层启动脚本、复制 `bin/` 和 `lib/`，并对运行包内的可执行文件和 `.so` 执行 `strip --strip-unneeded`。如果自动探测不到 strip 工具，可以通过 `--strip-tool /path/to/aarch64-strip` 指定。

部署到 X5：

```bash
ssh root@<x5-ip> "rm -rf /root/demo && mkdir -p /root/demo"
tar -C deploy_runtime -cf - . | ssh root@<x5-ip> "tar -xf - -C /root/demo"
ssh root@<x5-ip> "chmod +x /root/demo/cam_demo /root/demo/imu_reader_demo /root/demo/serial_port_demo /root/demo/bin/*"
```

板端目录结构：

```text
/root/demo/
├── cam_demo
├── imu_reader_demo
├── serial_port_demo
├── env.sh
├── bin/
│   ├── cam_demo
│   ├── imu_reader_demo
│   └── serial_port_demo
└── lib/
    ├── libicm42688.so
    ├── libsc132.so
    └── libprrtsp.so
```

默认运行方式：

```bash
cd /root/demo
./cam_demo
./imu_reader_demo
./serial_port_demo
```

顶层 `cam_demo`、`imu_reader_demo`、`serial_port_demo` 是启动脚本，会先设置：

```bash
LD_LIBRARY_PATH=/root/demo/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib
```

真实 ELF 在 `bin/` 下。如果要直接运行 `bin/` 下的 ELF，需要先加载环境：

```bash
cd /root/demo
. ./env.sh
./bin/cam_demo
```

三个 demo 都带有默认配置，普通功能验证时直接执行顶层脚本即可。需要修改帧率、码率、串口号或采样次数时，再通过命令行参数覆盖默认值。

## 4. SC132 四目相机 RTSP Demo

`cam_demo` 演示如何同时使用：

- `libsc132.so`：启动 SC132 四目相机，并通过 frame-set callback 获取配组后的 NV12 DMA 帧
- `libprrtsp.so`：把四路 NV12 帧送入 X5 编码器并输出 RTSP

三个 demo 可执行文件构建时会静态链接 `libstdc++` 和 `libgcc`，避免 X5 板端 `/usr/lib/libstdc++.so.6` 版本较旧导致 `GLIBCXX_* not found`。

交付库 `libsc132.so`、`libprrtsp.so` 和 `libicm42688.so` 已去掉 debug 信息，并通过导出符号表只保留公开头文件声明的 API。

注意：请保持 `cam_demo`、`include/` 和 `lib/` 中的二进制库版本匹配。不要混用系统目录或其他工程里的同名 `.so`，否则可能出现启动失败或运行时符号不匹配。

默认运行：

```bash
./cam_demo
```

当前 X5 镜像依赖系统 `cam-service` 初始化 camera/ISP 基线；运行 demo 前先确认该服务存在，不要同时运行多个相机应用：

```bash
/etc/init.d/S90cam-service start 2>/dev/null || true
pgrep -a cam-service
killall -q cam_demo 2>/dev/null || true
```

`--trigger-mode` 默认值是 `software_gpio`，对应当前四目相机外触发接线。普通使用不需要手动设置触发环境变量。

`--channels 1 --fps 60` 会自动选择单路 60fps 所需的相机配置。用户如果已经手动设置 `SC132_SENSOR_PROFILE`，demo 不会覆盖该环境变量。

部署时请整目录拷贝 `/root/demo` 运行包。顶层入口会设置 `LD_LIBRARY_PATH`，如果只拷贝 `bin/cam_demo` 或单个 `.so`，板端可能加载 `/usr/lib` 或 `/lib64` 下的系统库，难以复现 demo 交付环境。

如果 `. ./env.sh && ldd ./bin/cam_demo` 显示加载了 `/usr/lib/libsc132.so`，说明当前环境没有优先使用本目录 `lib/` 下的库。请确认 `env.sh`、`bin/`、`lib/` 来自同一个运行包。

常用参数：

```text
--channels <1-4>   启用相机路数，默认 4
--width <pixels>   图像宽度，默认 1088
--height <pixels>  图像高度，默认 1280
--fps <30|60>      相机和编码帧率，默认 60
--bps <kbps>       编码目标平均码率，单位 kbps，默认 2000；四路约 8Mbps 总码率
--url <path>       RTSP path，默认 /PRR
--trigger-mode <software_gpio|vin_lpwm|none> 触发输出模式，默认 software_gpio/GPIO417
--diagnostics      输出每路送帧耗时和时间戳 skew 诊断信息
--max-skew-ns <ns> timestamp skew 诊断阈值，默认 1000000；当前严格配组依据是归一化 frame_id
--frame-timeout-ms <ms> 帧组等待缺路帧的超时时间，默认 100
```

默认四路 RTSP 地址：

```text
rtsp://<x5-ip>:554/PRR
rtsp://<x5-ip>:555/PRR
rtsp://<x5-ip>:556/PRR
rtsp://<x5-ip>:557/PRR
```

默认 RTSP 端口固定为 `554/555/556/557`。camera 0/1/2/3 分别对应 `ch1/ch2/ch3/ch4`，交付例程不提供端口重映射参数。`libprrtsp.so` 还保留默认无编号通道，可用于单路示例推流；四目 demo 不使用默认通道，因此 `ch1` 可以占用 554 端口。不要在同一进程里同时把默认通道和 `ch1` 初始化到 554 端口。

相机回调后的处理流程：

1. `cam_demo` 使用 `VioCamInitmFrameSet()` 注册四目同步 frame-set callback。
2. `libsc132.so` 对四路相机帧做同步配组，配组成功后回调给 demo。
3. demo 在帧组回调里调用用户入口，并给每路 frame `retain` 后放入对应 RTSP 队列。
4. 队列满时等待后台线程释放空槽，不丢弃旧帧。
5. 后台线程从队列取帧，调用对应 `Rtsp_SendImg*_planes()` 推流。
6. 后台线程处理完成后调用 `sc132_frame_release()` 归还帧。

用户二次开发的四目同步入口在 `src/cam_demo.cpp` 的 `OnSynchronizedFrameSet()`。该函数收到的是同一个 `group_id` 下的四路帧，包含 `max_skew_ns`、每路 `camera_id`、`sequence`、`frame_id` 和 `timestamp_ns`；其中 `max_skew_ns` 是时间戳诊断值，不是当前配组放行条件。不要把裸指针保存到更长生命周期；如果要异步使用图像，请自行 `sc132_frame_retain()`，处理完成后 `sc132_frame_release()`。

日志字段：

- `seq`：每个相机通道独立递增的软件序号
- `group_id`：`libsc132.so` 生成的四目同步帧组序号
- `group_skew_ns`：当前帧组四路 timestamp 最大差值，单位 `ns`，用于诊断链路相位差
- `frame_id`：SC132/VIO 输出帧号；`libsc132.so` 内部按每路首帧归一化后的 frame index 做严格配组
- `camera_ts_ns`：相机帧时间戳，单位 `ns`；优先为 sensor/VIO 时间戳，fallback 为系统出帧时间
- `enqueue_timestamp_ns`：入队时 host steady clock 时间戳，单位 `ns`
- `full_waits`：队列满时回调等待空槽的次数，正常稳定推流时应长期为 `0`
- `pipeline_delay_ms`：当前帧从入队到完成 RTSP 送帧调用的耗时
- `send_avg_ms` / `send_max_ms`：开启 `--diagnostics` 后输出，表示统计周期内 `Rtsp_SendImg*_planes()` 调用耗时
- `rtsp_latest_skew_ms`：开启 `--diagnostics` 后输出，表示四路最近一次送出的相机时间戳最大差值

## 5. IMU 读取 Demo

默认运行：

```bash
./imu_reader_demo
```

常用调试示例：

```bash
./imu_reader_demo --sample-rate-hz 1000 --count 10
```

输出字段：

- `ts_ns`：host monotonic clock 时间戳，单位 `ns`
- `dt_ms`：相邻两帧时间戳差，单位 `ms`
- `temp_c`：温度，单位 `degC`
- `accel_mps2`：三轴加速度，单位 `m/s^2`
- `accel_norm_mps2`：三轴加速度模长，静止时通常接近 `9.81`
- `gyro_rps`：三轴角速度，单位 `rad/s`

说明：

- demo 默认使用 FIFO 模式
- FIFO 模式下，驱动按配置 ODR 展开连续时间戳，用于提供稳定 `dt`
- 当前时间戳不是 FSYNC 外部同步时间戳
- 回调函数运行在驱动采集线程中，实际项目里应避免在回调里做耗时操作

## 6. 串口通信 Demo

默认运行：

```bash
./serial_port_demo
```

默认配置使用 `/dev/ttyS1`、`115200`、`txrx` 模式。需要指定端口或模式时再增加参数，例如：

```bash
./serial_port_demo --port /dev/ttyS1 --mode tx --baud 115200 --text "hello-x5"
./serial_port_demo --port /dev/ttyS7 --mode rx --baud 115200
./serial_port_demo --port /dev/ttyS1 --mode txrx --baud 115200 --count 10 --text "ping"
./serial_port_demo --port /dev/ttyS7 --mode echo --baud 115200
```

常用参数：

```text
--port <path>             串口设备，默认 /dev/ttyS1
--baud <rate>             波特率，默认 115200
--mode <tx|rx|txrx|echo>  模式，默认 txrx
--count <n>               tx/txrx 表示发送次数，rx/echo 表示接收包数，0 表示持续运行
--interval-ms <ms>        发送间隔，默认 1000
--timeout-ms <ms>         接收超时，默认 200
--text <str>              发送文本前缀，默认 uart-demo
--no-newline              发送数据末尾不追加换行
```

## 7. 自动回归测试和评估标准

`scripts/cam_demo_regression.sh` 用于在开发机上通过 SSH 驱动 X5 板端执行四目 RTSP 自动回归。脚本不会默认停止已有相机进程；如果板端正在运行其他 `cam_demo`，脚本会直接失败退出，避免影响现场观察。

运行前提：

- X5 上已经部署好 `cam_demo` 和同级 `lib/`
- 板端 `/root/demo/env.sh` 能正确设置 `LD_LIBRARY_PATH`
- 开发机能 SSH 到 X5
- 开发机有 `nc` 更好；没有时脚本会退回 bash `/dev/tcp` 做端口探测
- 使用密码登录时需要 `sshpass`，也可以配置 SSH key 后不传密码

示例：

```bash
cd RoboBaton_4p_demo
X5_PASS=<password> scripts/cam_demo_regression.sh \
  --host <x5-ip>
```

专用测试环境里如果需要脚本清理旧进程，可以显式增加：

```bash
X5_PASS=<password> scripts/cam_demo_regression.sh \
  --host <x5-ip> \
  --kill-existing
```

默认测试参数：

- 运行时长：`25 s`
- 相机/编码帧率：`60 fps`
- 输入尺寸：`1088x1280`
- RTSP 端口：`554/555/556/557`

默认 PASS 标准：

| 项目 | 标准 |
|---|---|
| 动态库加载 | 设置 `LD_LIBRARY_PATH` 后，`ldd ./bin/cam_demo` 必须加载当前目录 `lib/libsc132.so` 和 `lib/libprrtsp.so` |
| RTSP 端口 | `554/555/556/557` 必须在启动窗口内全部可连接 |
| sensor 检测 | 至少检测到 4 个 `sc132gs-1280p` |
| 编码器初始化 | `Encode idx: 0..3, init successful` 至少各出现一次 |
| 致命错误 | 日志不能出现段错误、`undefined symbol`、`GLIBCXX`、`ret=-36`、`ret=-10`、`create_and_run_vflow failed` 等关键错误 |
| 帧率 | 每路最后一条 fps 不低于 `55`，且每路至少 3 个统计样本不低于 `55` |
| 帧组进展 | `group_id` 必须超过按运行时长和 fps 推导的最低值 |
| 队列阻塞 | `full_waits` 必须保持 `0` |
| 处理延迟 | `pipeline_delay_ms <= 80` |
| RTSP 送帧耗时 | `send_max_ms <= 120` |
| 时间戳诊断 | `group_skew_ns <= 1000000` |
| 帧号一致性 | 同一帧组四路 `frame_id` 相对 offset jitter 必须为 `0` |

可调阈值：

```bash
scripts/cam_demo_regression.sh --host <x5-ip> \
  --min-fps 55 \
  --min-good-fps-samples 3 \
  --max-pipeline-delay-ms 80 \
  --max-send-max-ms 120 \
  --max-group-skew-ns 1000000 \
  --min-group-id 500
```

评估说明：

- `sample_reason=frame-index` 作为 WARN 统计；只要 fps、`group_id`、队列和延迟达标，不直接判失败。
- `sample_reason=base-skew` 多发生在启动早期，是基准建立前清理旧队头；只要后续持续出帧，不直接判失败。
- 回归脚本只做“链路健康”和“推流端口可达”自动判定，不替代画面内容检查；如果要验证画面内容，仍需播放器或图像抓帧工具配合。
- 如果脚本提示已有 `cam_demo`，先人工确认是否可以停掉；不要在现场观察过程中随意使用 `--kill-existing`。

日志会保存到：

```text
RoboBaton_4p_demo/regression_logs/
```

该目录已被 `.gitignore` 忽略。

## 8. 运行约束

IMU 驱动库内部固定使用当前 X5 主板连接：

- SPI 设备节点：`/dev/spidev2.0`
- SPI mode：`0`
- SPI speed：`4 MHz`
- 默认读取模式：FIFO

串口 demo 不固定硬件连线，用户需要根据现场接线选择 `/dev/ttyS1`、`/dev/ttyS7` 或其他串口设备。

SC132 相机 demo 依赖 X5 板端 camera/vpf/hbmem/multimedia/FFmpeg/OpenSSL 等系统运行库，只适合在 X5 板端运行。开发机只用于交叉编译。

## 9. 常见问题

### 9.1 找不到 `.so`

确认目标目录是：

```text
/root/demo/
├── imu_reader_demo / serial_port_demo / cam_demo
├── env.sh
├── bin/
│   ├── imu_reader_demo
│   ├── serial_port_demo
│   └── cam_demo
└── lib/
    ├── libicm42688.so
    ├── libsc132.so
    └── libprrtsp.so
```

默认通过顶层脚本运行时会自动设置 `LD_LIBRARY_PATH`。如果直接运行 `bin/` 下的 ELF，先执行：

```bash
cd /root/demo
. ./env.sh
./bin/imu_reader_demo
```

### 9.2 IMU 启动失败

检查：

```bash
ls -l /dev/spidev2.0
./imu_reader_demo
```

常见原因：

- `/dev/spidev2.0` 不存在
- SPI 管脚被其他服务占用
- IMU 供电、焊接或设备树配置异常

### 9.3 串口没有数据

检查：

```bash
ls -l /dev/ttyS1 /dev/ttyS7
./serial_port_demo
```

常见原因：

- 端口选错
- 波特率不一致
- TX/RX 线序错误
- 对端没有发送数据

### 9.4 相机或 RTSP 启动失败

检查：

```bash
ls -l lib/libsc132.so lib/libprrtsp.so
. ./env.sh
ldd ./bin/cam_demo
./cam_demo
```

常见原因：

- SC132 四目相机硬件未连接或供电异常
- X5 设备树 / camera sensor profile 不匹配
- X5 multimedia 运行库缺失或版本不匹配
- `LD_LIBRARY_PATH` 未包含当前目录 `lib/`，或 `ldd ./bin/cam_demo` 没有优先加载本目录 `lib/libsc132.so` / `lib/libprrtsp.so`
- 替换了 `libsc132.so` 后没有重新核对内置 SC132 sensor 配置 ABI
- 系统 `cam-service` 未运行或状态异常；先执行 `/etc/init.d/S90cam-service start`
- 另一个相机应用仍在运行，占用了 camera/VIO 资源
- 默认 RTSP 端口 `554/555/556/557` 被其他进程占用
- 当前网络无法从开发机访问 X5 RTSP 端口

如果 `./cam_demo --channels 1 --fps 60` 在日志中返回 `ret=-36`，说明当前单路相机配置不匹配。清掉外部 `SC132_SENSOR_PROFILE` 后重试，或显式设置：

```bash
export SC132_SENSOR_PROFILE=sc132gs_linear_1088x1280_raw10_60fps_1lane
```

如果需要确认相机采集、队列或 RTSP 送帧是否存在延迟，可以临时使用：

```bash
./cam_demo --channels 4 --fps 60 --diagnostics
```

判断依据：

- 如果应用日志里的 `fps` 接近 60、`full_waits=0`、`pipeline_delay_ms` 只有数毫秒，但播放器某一路明显慢，问题更可能在 RTSP server/client 缓冲或播放器显示链路。
- 如果某一路 `rtsp_latest_skew_ms` 持续扩大到多帧，或 `send_max_ms` 长时间异常升高，再继续排查对应 RTSP context 或编码器调用。
- 如果日志持续打印 `[FRAME_SET] drops ... sample_reason=frame-index`，说明某路帧号追不上当前帧组，优先检查相机启动、触发和回调消费是否持续丢帧。
- 如果启动早期打印 `sample_reason=base-skew`，说明正在清理启动阶段旧帧；只要后续四路 fps 持续接近目标帧率，这属于恢复性清理。
- 如果 `group_skew_ns` 或 `rtsp_latest_skew_ms` 长期接近一个帧周期，说明四路输出节点存在固定相位差，应继续检查外触发、相机启动顺序和板端负载。
