# X5 IMU And UART Open Source Demo

Chinese version: [README.md](README.md)

This is the minimal open-source demo package for users. It includes the IMU reader demo, the UART communication demo, the public header, and the binary driver library. It does not include the ICM-42688-P driver implementation source code.

## 1. Directory Layout

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

## 2. Build

This demo is intended to be cross-compiled on a development host and only run on the X5 board. Native compilation on the X5 board is not required or recommended.

Build prerequisites:

- X5 aarch64 cross-compilation toolchain
- CMake
- Toolchain file from the X5 SDK

The toolchain file path below is only an example. Replace it with the actual path in your environment:

```bash
cd open_source_demo
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake
cmake --build build_x5 -j
```

The toolchain file used during project validation was:

```text
/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
```

Generated binaries:

- `build_x5/imu_reader_demo`
- `build_x5/serial_port_demo`

Check the target architecture:

```bash
file build_x5/imu_reader_demo
file build_x5/serial_port_demo
file lib/libicm42688_x5.so
```

The expected output should contain `ARM aarch64`.

If the cross-compilation toolchain is not available, the demo cannot be rebuilt. In that case, deploy the prebuilt `imu_reader_demo`, `serial_port_demo`, and `lib/libicm42688_x5.so` to the board and run them directly.

## 3. Deploy

Recommended target directory on X5:

```bash
ssh root@<x5-ip> "mkdir -p /userdata/imu_demo/lib"
scp build_x5/imu_reader_demo root@<x5-ip>:/userdata/imu_demo/
scp build_x5/serial_port_demo root@<x5-ip>:/userdata/imu_demo/
scp lib/libicm42688_x5.so root@<x5-ip>:/userdata/imu_demo/lib/
```

Run on X5:

```bash
cd /userdata/imu_demo
chmod +x imu_reader_demo serial_port_demo
```

## 4. IMU Reader Demo

Read 10 frames:

```bash
./imu_reader_demo --sample-rate-hz 1000 --count 10
```

Read continuously:

```bash
./imu_reader_demo --sample-rate-hz 1000
```

Output fields:

- `ts_ns`: host monotonic clock timestamp in `ns`
- `dt_ms`: timestamp delta between adjacent frames in `ms`
- `temp_c`: temperature in `degC`
- `accel_mps2`: 3-axis acceleration in `m/s^2`
- `accel_norm_mps2`: acceleration norm, typically close to `9.81` when stationary
- `gyro_rps`: 3-axis angular velocity in `rad/s`

Notes:

- The demo uses FIFO mode by default.
- In FIFO mode, the driver expands sample timestamps by the configured ODR to provide stable `dt`.
- The timestamp is not an external FSYNC timestamp.
- The callback runs in the driver's acquisition thread. Avoid blocking or heavy work inside the callback in real applications.

## 5. UART Communication Demo

Transmit:

```bash
./serial_port_demo --port /dev/ttyS1 --mode tx --baud 115200 --text "hello-x5"
```

Receive:

```bash
./serial_port_demo --port /dev/ttyS7 --mode rx --baud 115200
```

Transmit and wait for a reply:

```bash
./serial_port_demo --port /dev/ttyS1 --mode txrx --baud 115200 --count 10 --text "ping"
```

Echo received bytes:

```bash
./serial_port_demo --port /dev/ttyS7 --mode echo --baud 115200
```

Common options:

```text
--port <path>             Serial device, default /dev/ttyS1
--baud <rate>             Baud rate, default 115200
--mode <tx|rx|txrx|echo>  Mode, default txrx
--count <n>               TX/TXRX rounds or RX/ECHO packets, 0 means forever
--interval-ms <ms>        TX interval, default 1000
--timeout-ms <ms>         RX timeout, default 200
--text <str>              TX payload prefix, default uart-demo
--no-newline              Do not append newline to the TX payload
```

## 6. Runtime Constraints

The IMU driver library is fixed for the current X5 mainboard connection:

- SPI device node: `/dev/spidev2.0`
- SPI mode: `0`
- SPI speed: `4 MHz`
- Default read mode: FIFO

The UART demo does not assume fixed wiring. Select `/dev/ttyS1`, `/dev/ttyS7`, or another serial device according to the actual hardware connection.

## 7. Troubleshooting

### 7.1 `libicm42688_x5.so` Not Found

Confirm the target directory layout:

```text
/userdata/imu_demo/
├── imu_reader_demo
└── lib/
    └── libicm42688_x5.so
```

If the library is not under `./lib/`, use:

```bash
LD_LIBRARY_PATH=/path/to/lib ./imu_reader_demo --sample-rate-hz 1000 --count 10
```

### 7.2 IMU Startup Failure

Check:

```bash
ls -l /dev/spidev2.0
./imu_reader_demo --sample-rate-hz 1000 --count 10
```

Common causes:

- `/dev/spidev2.0` does not exist
- SPI pins are occupied by another service
- IMU power, soldering, or device-tree configuration is incorrect

### 7.3 No UART Data

Check:

```bash
ls -l /dev/ttyS1 /dev/ttyS7
./serial_port_demo --port /dev/ttyS1 --mode rx --baud 115200
```

Common causes:

- Wrong port
- Baud rate mismatch
- TX/RX wires are swapped incorrectly or not connected
- The peer device is not transmitting data
