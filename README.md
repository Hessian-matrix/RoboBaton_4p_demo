# X5 IMU And UART Open Source Demo

English version: [README_EN.md](README_EN.md)

这是给用户交付的最小开源 demo。它只包含 IMU 读取示例、串口通信示例、公开头文件和二进制驱动库，不包含 ICM-42688-P 驱动实现源码。

## 1. 目录结构

```text
open_source_demo/
├── CMakeLists.txt
├── README.md
├── README_EN.md
├── include/
│   └── icm42688_x5/
│       └── icm42688_driver.h
├── lib/
│   └── libicm42688_x5.so
└── src/
    ├── imu_reader_demo.cpp
    └── serial_port_demo.cpp
```

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

本项目验证时使用的工具链文件路径是：

```text
/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
```

生成文件：

- `build_x5/imu_reader_demo`
- `build_x5/serial_port_demo`

检查架构：

```bash
file build_x5/imu_reader_demo
file build_x5/serial_port_demo
file lib/libicm42688_x5.so
```

期望输出包含 `ARM aarch64`。

如果没有交叉编译工具链，则不能重新编译 demo，只能使用已经编译好的 `imu_reader_demo`、`serial_port_demo` 和 `lib/libicm42688_x5.so` 部署到板端运行。

## 3. 部署

建议部署到 X5 的 `/userdata/imu_demo/`：

```bash
ssh root@<x5-ip> "mkdir -p /userdata/imu_demo/lib"
scp build_x5/imu_reader_demo root@<x5-ip>:/userdata/imu_demo/
scp build_x5/serial_port_demo root@<x5-ip>:/userdata/imu_demo/
scp lib/libicm42688_x5.so root@<x5-ip>:/userdata/imu_demo/lib/
```

在 X5 上执行：

```bash
cd /userdata/imu_demo
chmod +x imu_reader_demo serial_port_demo
```

## 4. IMU 读取 Demo

读取 10 帧：

```bash
./imu_reader_demo --sample-rate-hz 1000 --count 10
```

持续读取：

```bash
./imu_reader_demo --sample-rate-hz 1000
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

## 5. 串口通信 Demo

发送：

```bash
./serial_port_demo --port /dev/ttyS1 --mode tx --baud 115200 --text "hello-x5"
```

接收：

```bash
./serial_port_demo --port /dev/ttyS7 --mode rx --baud 115200
```

发送并等待回包：

```bash
./serial_port_demo --port /dev/ttyS1 --mode txrx --baud 115200 --count 10 --text "ping"
```

收到即回显：

```bash
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

## 6. 运行约束

IMU 驱动库内部固定使用当前 X5 主板连接：

- SPI 设备节点：`/dev/spidev2.0`
- SPI mode：`0`
- SPI speed：`4 MHz`
- 默认读取模式：FIFO

串口 demo 不固定硬件连线，用户需要根据现场接线选择 `/dev/ttyS1`、`/dev/ttyS7` 或其他串口设备。

## 7. 常见问题

### 7.1 找不到 libicm42688_x5.so

确认目标目录是：

```text
/userdata/imu_demo/
├── imu_reader_demo
└── lib/
    └── libicm42688_x5.so
```

如果库不在 `./lib/` 下，可以临时使用：

```bash
LD_LIBRARY_PATH=/path/to/lib ./imu_reader_demo --sample-rate-hz 1000 --count 10
```

### 7.2 IMU 启动失败

检查：

```bash
ls -l /dev/spidev2.0
./imu_reader_demo --sample-rate-hz 1000 --count 10
```

常见原因：

- `/dev/spidev2.0` 不存在
- SPI 管脚被其他服务占用
- IMU 供电、焊接或设备树配置异常

### 7.3 串口没有数据

检查：

```bash
ls -l /dev/ttyS1 /dev/ttyS7
./serial_port_demo --port /dev/ttyS1 --mode rx --baud 115200
```

常见原因：

- 端口选错
- 波特率不一致
- TX/RX 线序错误
- 对端没有发送数据
