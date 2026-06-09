# X5 SC132 4-Camera, IMU, And UART Demo

Chinese version: [README.md](README.md)

This is the minimal user-facing demo package. It includes the SC132 4-camera RTSP demo, IMU reader demo, UART communication demo, public headers, and binary driver libraries. It does not include the underlying driver implementation source code.

## 1. Directory Layout

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

`cam_demo.cpp` keeps the main flow and user extension hooks. Command-line parsing, RTSP wrapping, frame queues, and background streaming are split into `cam_demo_config.*`, `cam_demo_rtsp.*`, and `cam_demo_pipeline.*` for easier reading.

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

You can also build one demo target at a time:

```bash
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_cam_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_imu_reader_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_serial_port_demo.sh
```

The scripts use `build_x5/` by default. Set `BUILD_DIR=/path/to/build_x5` if a different build directory is required.

The toolchain file used during project validation was:

```text
/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
```

Generated binaries:

- `build_x5/imu_reader_demo`
- `build_x5/serial_port_demo`
- `build_x5/cam_demo`

Check the target architecture:

```bash
file build_x5/imu_reader_demo
file build_x5/serial_port_demo
file build_x5/cam_demo
file lib/libicm42688.so
file lib/libsc132.so
file lib/libprrtsp.so
```

The expected output should contain `ARM aarch64`.

If the cross-compilation toolchain is not available, the demo cannot be rebuilt. In that case, deploy the prebuilt binaries and the matching libraries under `lib/` to the board and run them directly.

## 3. Deploy

Recommended target directory on X5: `/root/demo/`. First generate the runtime package on the development host:

```bash
cd open_source_demo
scripts/package_runtime.sh --build-dir build_x5 --output-dir deploy_runtime
```

`package_runtime.sh` generates the top-level launchers, copies `bin/` and `lib/`, and runs `strip --strip-unneeded` on the runtime-package executables and `.so` files. If the strip tool cannot be auto-detected, pass `--strip-tool /path/to/aarch64-strip`.

Deploy it to X5:

```bash
ssh root@<x5-ip> "rm -rf /root/demo && mkdir -p /root/demo"
tar -C deploy_runtime -cf - . | ssh root@<x5-ip> "tar -xf - -C /root/demo"
ssh root@<x5-ip> "chmod +x /root/demo/cam_demo /root/demo/imu_reader_demo /root/demo/serial_port_demo /root/demo/bin/*"
```

Runtime layout on X5:

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

Default run commands:

```bash
cd /root/demo
./cam_demo
./imu_reader_demo
./serial_port_demo
```

The top-level `cam_demo`, `imu_reader_demo`, and `serial_port_demo` files are launcher scripts. They set:

```bash
LD_LIBRARY_PATH=/root/demo/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib
```

The real ELF binaries are under `bin/`. To run a `bin/` binary directly, load the environment first:

```bash
cd /root/demo
. ./env.sh
./bin/cam_demo
```

All three demos provide default configurations. For normal bring-up, run the top-level launcher directly. Use command-line options only when changing FPS, bitrate, serial port, sample count, or other runtime parameters.

## 4. SC132 4-Camera RTSP Demo

`cam_demo` demonstrates how to use:

- `libsc132.so`: starts the SC132 4-camera pipeline and provides synchronized NV12 DMA frames through a frame-set callback
- `libprrtsp.so`: sends the four NV12 streams to the X5 encoder and publishes RTSP streams

All three demo executables statically link `libstdc++` and `libgcc` to avoid `GLIBCXX_* not found` on boards with an older `/usr/lib/libstdc++.so.6`.

The delivered `libsc132.so`, `libprrtsp.so`, and `libicm42688.so` are stripped and use export-symbol control so only public-header APIs remain visible.

Important: keep `cam_demo`, `include/`, and the libraries under `lib/` from the same package version. Do not mix same-named `.so` files from system directories or other projects, or startup/runtime symbol mismatches may occur.

Default run:

```bash
./cam_demo
```

The current X5 image uses the system `cam-service` to initialize the camera/ISP baseline. Before running the demo, make sure the service is present, and do not run multiple camera applications at the same time:

```bash
/etc/init.d/S90cam-service start 2>/dev/null || true
pgrep -a cam-service
killall -q cam_demo 2>/dev/null || true
```

`--trigger-mode` defaults to `software_gpio`, matching the delivered 4-camera external trigger wiring. Normal usage does not require setting trigger-related environment variables.

`--channels 1 --fps 60` automatically selects the camera profile required for single-channel 60fps. If the user already set `SC132_SENSOR_PROFILE`, the demo does not override it.

Deploy the complete `/root/demo` runtime package. The top-level launchers set `LD_LIBRARY_PATH`; if only `bin/cam_demo` or a single `.so` is copied, the board may load system libraries from `/usr/lib` or `/lib64`, which is not the verified demo environment.

If `. ./env.sh && ldd ./bin/cam_demo` shows `/usr/lib/libsc132.so`, the local `lib/` directory is not taking precedence. Make sure `env.sh`, `bin/`, and `lib/` come from the same runtime package.

Common options:

```text
--channels <1-4>   Enabled camera count, default 4
--width <pixels>   Frame width, default 1088
--height <pixels>  Frame height, default 1280
--fps <30|60>      Camera and encoder fps, default 60
--bps <kbps>       Target average encoder bitrate in kbps, default 2000; four streams are about 8Mbps total
--url <path>       RTSP path, default /PRR
--trigger-mode <software_gpio|vin_lpwm|none> Trigger output mode, default software_gpio/GPIO417
--diagnostics      Print per-channel send timing and timestamp skew diagnostics
--max-skew-ns <ns> Timestamp skew diagnostic threshold, default 1000000; strict grouping currently uses normalized frame_id
--frame-timeout-ms <ms> Timeout for waiting for missing channels in a frame set, default 100
```

Default RTSP URLs:

```text
rtsp://<x5-ip>:554/PRR
rtsp://<x5-ip>:555/PRR
rtsp://<x5-ip>:556/PRR
rtsp://<x5-ip>:557/PRR
```

The default RTSP ports are fixed to `554/555/556/557`. Cameras 0/1/2/3 correspond to `ch1/ch2/ch3/ch4`; the delivered demo does not expose a port remapping option. `libprrtsp.so` also keeps the default unnumbered channel for single-stream examples. The four-camera demo does not use that default channel, so `ch1` can use port 554. Do not initialize the default channel and `ch1` to port 554 in the same process.

Frame flow:

1. `cam_demo` registers a synchronized frame-set callback through `VioCamInitmFrameSet()`.
2. `libsc132.so` synchronizes the four camera frames and emits a frame-set callback after grouping succeeds.
3. The demo calls the user hook inside the frame-set callback, then retains each frame and pushes it into the corresponding RTSP queue.
4. If a queue is full, the callback waits for a free slot instead of dropping older frames.
5. Worker threads pop frames and call the matching `Rtsp_SendImg*_planes()` function.
6. Worker threads call `sc132_frame_release()` after processing.

The user development hook for synchronized four-camera data is `OnSynchronizedFrameSet()` in `src/cam_demo.cpp`. The callback receives four frames under one `group_id`, with `max_skew_ns`, per-camera `camera_id`, `sequence`, `frame_id`, and `timestamp_ns`; `max_skew_ns` is a timestamp diagnostic value, not the current frame-set release condition. Do not keep raw frame pointers beyond the callback lifetime unless you call `sc132_frame_retain()` and later call `sc132_frame_release()`.

Log fields:

- `seq`: per-camera software sequence
- `group_id`: synchronized frame-set sequence generated by `libsc132.so`
- `group_skew_ns`: maximum timestamp skew within the frame set, in `ns`, used to diagnose pipeline phase offset
- `frame_id`: SC132/VIO output frame id; `libsc132.so` strictly groups frames by each camera's normalized frame index
- `camera_ts_ns`: camera frame timestamp in `ns`; sensor/VIO timestamp first, system output timestamp as fallback
- `full_waits`: number of times the callback waited for a full queue; this should remain `0` in stable streaming
- `pipeline_delay_ms`: time from enqueue to RTSP send completion
- `send_avg_ms` / `send_max_ms`: `Rtsp_SendImg*_planes()` call timing when `--diagnostics` is enabled
- `rtsp_latest_skew_ms`: timestamp skew across the latest sent frames when `--diagnostics` is enabled

## 5. IMU Reader Demo

Default run:

```bash
./imu_reader_demo
```

Common debug example:

```bash
./imu_reader_demo --sample-rate-hz 1000 --count 10
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

## 6. UART Communication Demo

Default run:

```bash
./serial_port_demo
```

The default configuration uses `/dev/ttyS1`, `115200`, and `txrx` mode. Add options only when selecting a different port or mode, for example:

```bash
./serial_port_demo --port /dev/ttyS1 --mode tx --baud 115200 --text "hello-x5"
./serial_port_demo --port /dev/ttyS7 --mode rx --baud 115200
./serial_port_demo --port /dev/ttyS1 --mode txrx --baud 115200 --count 10 --text "ping"
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

## 7. Automated Regression Test And Criteria

`scripts/cam_demo_regression.sh` runs the 4-camera RTSP regression from the development host over SSH. It does not stop existing camera processes by default. If another `cam_demo` is already running, the script fails immediately to avoid interrupting a live visual check.

Prerequisites:

- `cam_demo` and its sibling `lib/` directory are already deployed on the X5 board
- `/root/demo/env.sh` correctly sets `LD_LIBRARY_PATH` on the board
- The development host can SSH into the X5 board
- `nc` is recommended on the development host; if missing, the script falls back to bash `/dev/tcp`
- Password-based SSH needs `sshpass`; SSH key auth works without a password argument

Example:

```bash
cd RoboBaton_4p_demo
X5_PASS=<password> scripts/cam_demo_regression.sh \
  --host <x5-ip>
```

In a dedicated test environment, allow the script to stop old camera processes explicitly:

```bash
X5_PASS=<password> scripts/cam_demo_regression.sh \
  --host <x5-ip> \
  --kill-existing
```

Default test setup:

- Runtime: `25 s`
- Camera/encoder FPS: `60 fps`
- Input size: `1088x1280`
- RTSP ports: `554/555/556/557`

Default PASS criteria:

| Item | Criterion |
|---|---|
| Shared library loading | With `LD_LIBRARY_PATH` set, `ldd ./bin/cam_demo` must load local `lib/libsc132.so` and `lib/libprrtsp.so` |
| RTSP ports | `554/555/556/557` must all accept TCP connections within the startup window |
| Sensor detection | At least 4 `sc132gs-1280p` sensors detected |
| Encoder init | `Encode idx: 0..3, init successful` appears for all four encoders |
| Fatal errors | No segmentation fault, `undefined symbol`, `GLIBCXX`, `ret=-36`, `ret=-10`, or `create_and_run_vflow failed` |
| FPS | Each channel's last FPS is at least `55`, with at least 3 samples per channel at or above `55` |
| Frame-set progress | `group_id` exceeds the runtime/FPS-derived minimum |
| Queue blocking | `full_waits` remains `0` |
| Pipeline delay | `pipeline_delay_ms <= 80` |
| RTSP send cost | `send_max_ms <= 120` |
| Timestamp diagnostic | `group_skew_ns <= 1000000` |
| Frame-id consistency | Per-frame-set `frame_id` offset jitter across four cameras must be `0` |

Adjust thresholds:

```bash
scripts/cam_demo_regression.sh --host <x5-ip> \
  --min-fps 55 \
  --min-good-fps-samples 3 \
  --max-pipeline-delay-ms 80 \
  --max-send-max-ms 120 \
  --max-group-skew-ns 1000000 \
  --min-group-id 500
```

Interpretation:

- `sample_reason=frame-index` is reported as a warning. It does not fail the run if FPS, `group_id`, queue, and delay checks pass.
- `sample_reason=base-skew` usually appears only during startup cleanup before fixing the frame-id bases. It does not fail the run if streaming continues normally.
- The regression script checks link health and RTSP port reachability. It does not replace visual/content inspection; use a player or frame-grab tool when image content needs to be checked.
- If the script reports an existing `cam_demo`, confirm manually before stopping it. Do not use `--kill-existing` during active visual inspection.

Logs are saved under:

```text
RoboBaton_4p_demo/regression_logs/
```

This directory is ignored by `.gitignore`.

## 8. Runtime Constraints

The IMU driver library is fixed for the current X5 mainboard connection:

- SPI device node: `/dev/spidev2.0`
- SPI mode: `0`
- SPI speed: `4 MHz`
- Default read mode: FIFO

The UART demo does not assume fixed wiring. Select `/dev/ttyS1`, `/dev/ttyS7`, or another serial device according to the actual hardware connection.

The SC132 camera demo depends on X5 system runtime libraries such as camera, vpf, hbmem, multimedia, FFmpeg, and OpenSSL. It is intended to run on the X5 board only; the development host is only used for cross-compilation.

## 9. Troubleshooting

### 9.1 Shared Library Not Found

Confirm the target directory layout:

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

The top-level launchers set `LD_LIBRARY_PATH` automatically. If you run a `bin/` ELF directly, run:

```bash
cd /root/demo
. ./env.sh
./bin/imu_reader_demo --sample-rate-hz 1000 --count 10
```

### 9.2 IMU Startup Failure

Check:

```bash
ls -l /dev/spidev2.0
./imu_reader_demo
```

Common causes:

- `/dev/spidev2.0` does not exist
- SPI pins are occupied by another service
- IMU power, soldering, or device-tree configuration is incorrect

### 9.3 No UART Data

Check:

```bash
ls -l /dev/ttyS1 /dev/ttyS7
./serial_port_demo
```

Common causes:

- Wrong port
- Baud rate mismatch
- TX/RX wires are swapped incorrectly or not connected
- The peer device is not transmitting data

### 9.4 Camera Or RTSP Startup Failure

Check:

```bash
ls -l lib/libsc132.so lib/libprrtsp.so
. ./env.sh
ldd ./bin/cam_demo
./cam_demo
```

Common causes:

- SC132 camera hardware is not connected or not powered
- X5 device tree or camera sensor profile does not match the hardware
- X5 multimedia runtime libraries are missing or incompatible
- `LD_LIBRARY_PATH` does not include the local `lib/`, or `ldd ./bin/cam_demo` does not load the local `lib/libsc132.so` / `lib/libprrtsp.so`
- `libsc132.so` was replaced without rechecking the built-in SC132 sensor configuration ABI
- The system `cam-service` is not running or is in a bad state; start it with `/etc/init.d/S90cam-service start`
- Another camera application is still running and occupies camera/VIO resources
- Default RTSP ports `554/555/556/557` are already occupied
- The development host cannot reach the X5 RTSP ports over the network

If `./cam_demo --channels 1 --fps 60` returns `ret=-36`, the current single-channel camera profile does not match. Unset the external `SC132_SENSOR_PROFILE` and retry, or set it explicitly:

```bash
export SC132_SENSOR_PROFILE=sc132gs_linear_1088x1280_raw10_60fps_1lane
```

For timing diagnostics:

```bash
./cam_demo --channels 4 --fps 60 --diagnostics
```

Interpretation:

- If `fps` is close to 60, `full_waits=0`, and `pipeline_delay_ms` is only a few milliseconds while one player view is visibly slower, the delay is more likely in the RTSP server/client buffer or display path.
- If one channel's `rtsp_latest_skew_ms` keeps growing by multiple frames, or `send_max_ms` stays unusually high, continue checking that RTSP context or encoder call path.
- If logs continuously print `[FRAME_SET] drops ... sample_reason=frame-index`, one frame-id stream is falling behind the current frame set. Check camera startup, trigger stability, and callback consumption first.
- If logs print `sample_reason=base-skew` during startup, stale startup frames are being cleaned up. This is recovery cleanup as long as the later per-channel FPS stays near the target.
- If `group_skew_ns` or `rtsp_latest_skew_ms` stays close to one frame period, continue checking external trigger stability, camera startup order, and board load.
