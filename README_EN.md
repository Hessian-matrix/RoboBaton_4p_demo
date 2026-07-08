# X5 SC132 4-Camera, IMU, And UART Demo

Chinese version: [README.md](README.md)
![alt text](image/4P_Cam.png)

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

This repository treats `RoboBaton_4p_demo/demo/` as the board-side runtime update package. Users can copy the contents of `demo/` directly to `/root/demo/` on X5.

After code or shared-library changes, maintainers should rebuild the dependent libraries and refresh `demo/` on the development host:

```bash
cd /root/x5/4cam
scripts/build_sc132.sh
scripts/build_rtsp_so_mp4.sh

cd RoboBaton_4p_demo
scripts/package_runtime.sh
```

`scripts/package_runtime.sh` reads from `./build_x5` and `./lib` by default, then writes the runtime package to `./demo`. Keep `demo/` in the public repository or release artifact; if `demo/*` is ignored again, users will not receive the update package from the repository.

The runtime package contains the top-level launchers, `env.sh`, `bin/`, and `lib/`. Deploy the complete contents of `demo/` to the board. Do not copy only one executable or one `.so` file.

Deploy it to X5:

```bash
ssh root@<x5-ip> "rm -rf /root/demo && mkdir -p /root/demo"
tar -C demo -cf - . | ssh root@<x5-ip> "tar -xf - -C /root/demo"
ssh root@<x5-ip> "chmod +x /root/demo/cam_demo /root/demo/imu_reader_demo /root/demo/serial_port_demo /root/demo/bin/*"
```

Note: copy the contents of `demo/`, not the outer `demo/` directory itself; the board should not contain `/root/demo/demo/`.

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

The three demo executables are linked for the X5 runtime environment. Keep `cam_demo`, `include/`, and the libraries under `lib/` from the same package version. Do not mix same-named `.so` files from system directories or other projects, or startup/runtime symbol mismatches may occur.

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

`--trigger-mode` defaults to `software_gpio`, matching the delivered 4-camera external trigger wiring. For normal use, run `./cam_demo` directly; it starts the fixed four-camera, 60fps, upright `1280x1088` output path.

Deploy the complete `/root/demo` runtime package. The top-level launchers set `LD_LIBRARY_PATH`; if only `bin/cam_demo` or a single `.so` is copied, the board may load system libraries instead of the package libraries.

Common options:

```text
--width <pixels>   Frame width, default 1280
--height <pixels>  Frame height, default 1088
--fps <30|60>      Camera and encoder fps, default 60
--rotate <0|90|180|270> Output rotation, default 0; 180 is limited to 30fps and is not supported at 60fps
--bps <kbps>       Target average encoder bitrate in kbps, default 4000; H.264 + P-frame GOP motion-quality baseline, can be overridden for bandwidth/quality trade-offs
--url <path>       RTSP path, default /PRR
--trigger-mode <software_gpio|vin_lpwm|none> Trigger output mode, default software_gpio/GPIO417
--diagnostics      Print per-channel send timing and timestamp skew diagnostics
--max-skew-ns <ns> Timestamp skew diagnostic threshold, default 1000000; after synchronized grouping, all four exposed frame_id values match exactly
--frame-timeout-ms <ms> Timeout for waiting for missing channels in a frame set, default 100
```

Limit: default `./cam_demo` uses the fixed four-camera, 60fps, upright `1280x1088` output path. `--rotate 180` is supported only in the reduced-load 30fps mode and is not supported at 60fps.

Default RTSP URLs:

```text
rtsp://<x5-ip>:554/PRR
rtsp://<x5-ip>:555/PRR
rtsp://<x5-ip>:556/PRR
rtsp://<x5-ip>:557/PRR
```

The default RTSP ports are fixed to `554/555/556/557`. Cameras 0/1/2/3 correspond to the four output streams. The delivered demo does not expose a port remapping option.

### 4.1 Hardware check: single-sensor capture

If the four-camera demo fails to start, one stream has no image, or the FPC/I2C/MIPI connection is suspected, start one physical sensor at a time. This mode is for hardware diagnosis only; normal operation still uses `./cam_demo` for the fixed four-camera path.

Stop other camera processes before testing:

```bash
cd /root/demo
killall -q cam_demo 2>/dev/null || true
/etc/init.d/S90cam-service start 2>/dev/null || true
```

Start one physical camera id on the board:

```bash
./cam_demo --camera-id 0 --diagnostics   # cam0 -> rtsp://<x5-ip>:554/PRR
./cam_demo --camera-id 1 --diagnostics   # cam1 -> rtsp://<x5-ip>:555/PRR
./cam_demo --camera-id 2 --diagnostics   # cam2 -> rtsp://<x5-ip>:556/PRR
./cam_demo --camera-id 3 --diagnostics   # cam3 -> rtsp://<x5-ip>:557/PRR
```

Run only one `cam_demo` process at a time. Before switching to another sensor, press `Ctrl-C` or run:

```bash
killall -q cam_demo 2>/dev/null || true
```

Use `ffprobe` or an RTSP player on the development machine to confirm video. The example below checks cam0; use ports `555/556/557` for the other sensors:

```bash
ffprobe -v error -rtsp_transport tcp \
  -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://<x5-ip>:554/PRR
```

Expected output includes:

```text
codec_name=h264
width=1280
height=1088
avg_frame_rate=60/1
```

Diagnosis guide:

- If the board log prints `Found sensor_name:sc132gs-1280p` and `ffprobe` receives an H.264 stream, that sensor plus its I2C, MIPI/VIN, and RTSP path are basically healthy.
- If only one `--camera-id` fails, check that camera connector, FPC cable, power, and cable orientation first.
- If all four sensors work individually but the default four-camera mode fails, check the four-camera trigger wiring, GPIO417 external trigger, `cam-service`, and whether another camera process is using the hardware.

Single-sensor diagnosis supports only `--camera-id 0/1/2/3`; do not use this mode to validate two-camera or three-camera combinations.

Frame flow:

1. `cam_demo` registers the synchronized four-camera callback through the `libsc132.so` frame-set API.
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
- `frame_id`: synchronized frame-set id; all four frames under the same `group_id` must expose exactly the same value
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

Example:

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

interface wire sequence:
![alt text](image/UART.png)

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

## 7. Quick Verification After Deployment

After deployment, first confirm all three demos can print their help text:

```bash
cd /root/demo
./cam_demo --help
./imu_reader_demo --help
./serial_port_demo --help
```

Camera demo check:

```bash
cd /root/demo
/etc/init.d/S90cam-service start 2>/dev/null || true
pgrep -a cam-service
./cam_demo
```

After the demo starts, open the four RTSP streams with a player or RTSP client:

```text
rtsp://<x5-ip>:554/PRR
rtsp://<x5-ip>:555/PRR
rtsp://<x5-ip>:556/PRR
rtsp://<x5-ip>:557/PRR
```

Basic pass criteria:

- All four RTSP URLs connect and keep streaming.
- All four images are visible, with no black screen, obvious mosaic, or obvious freeze.
- The log reports per-camera `fps` close to the target frame rate.
- The log keeps `full_waits` at `0`.
- No obvious error, crash, or repeated camera restart appears.

## 8. Runtime Constraints

The IMU demo uses the current X5 mainboard connection by default:

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
- The system `cam-service` is not running or is in a bad state; start it with `/etc/init.d/S90cam-service start`
- Another camera application is still running and occupies camera/VIO resources
- Default RTSP ports `554/555/556/557` are already occupied
- The development host cannot reach the X5 RTSP ports over the network

This demo is designed for fixed four-camera operation and does not provide 2-camera or 3-camera partial-start modes.

For timing diagnostics:

```bash
./cam_demo --diagnostics
```

Interpretation:

- If `fps` is close to 60 and `full_waits=0`, but one player view is visibly slower, the delay is more likely in the RTSP client buffer or display path.
- If `send_max_ms` stays unusually high, continue checking that RTSP or encoder path.
- If `group_skew_ns` stays close to one frame period, continue checking external trigger stability, camera startup order, and board load.
