# X5 SC132 4-Camera, IMU, And UART Demo

Chinese version: [README.md](README.md)
![alt text](image/4P_Cam.png)

This is the minimal user-facing demo package. It includes the joint `sensor_demo`, the SC132 4-camera RTSP demo, the standalone GPIO395 INT1 IMU reader demo, the UART communication demo, public headers, and binary driver libraries. It does not include the underlying driver implementation source code.

## 1. Directory Layout

```text
open_source_demo/
├── CMakeLists.txt
├── README.md / README_EN.md
├── demo/                    # Runtime package deployable to X5 /root/demo
│   ├── cam_demo / sensor_demo / imu_reader_demo / serial_port_demo
│   ├── env.sh / manifest.sha256
│   ├── bin/                 # AArch64 executables
│   └── lib/                 # Shared libraries matched to the runtime package
├── image/                   # Wiring images used by the README files
├── include/
│   ├── icm42688_driver.h
│   ├── sc132camera.h
│   └── prrtsp_v2.h
├── lib/                     # Delivered libraries used for source cross-builds
├── scripts/
│   ├── build_cam_demo.sh
│   ├── build_sensor_demo.sh
│   ├── build_imu_reader_demo.sh
│   ├── build_serial_port_demo.sh
│   ├── cam_demo_regression.sh
│   ├── package_runtime.sh
│   └── verify_runtime_package.py
└── src/
    ├── cam_demo.cpp / sensor_demo.cpp
    ├── cam_demo_common.* / cam_demo_config.*
    ├── cam_demo_pipeline.* / cam_demo_rtsp.*
    ├── imu_reader_demo.cpp
    └── serial_port_demo.cpp
```

`cam_demo.cpp` keeps the main flow and user extension hooks. Command-line parsing, RTSP wrapping, frame queues, and background streaming are split into `cam_demo_config.*`, `cam_demo_rtsp.*`, and `cam_demo_pipeline.*` for easier reading.

This public repository does not contain the internal `tests/` directory or release checklist. When integrated into the top-level `4cam` workspace, those maintainer assets live under `tests/robobaton_4p_demo/`; they are not part of the user source delivery or the board-side `demo/` package. `build_x5/`, `.package-build-*`, `regression_logs/`, and Python caches are local generated artifacts and are not release content.

## 2. Build

This demo is intended to be cross-compiled on a development host and only run on the X5 board. Native compilation on the X5 board is not required or recommended.

Build prerequisites:

- X5 aarch64 cross-compilation toolchain (included in the companion archive)
- CMake (a host-side tool; **not included in the archive**, install it separately)
- Toolchain file from the X5 SDK (included in the companion archive)

Companion archive:

```text
<4cam-repo-root>/cross_compile_toolchain/x5_4cam_cross_toolchain_20260708.tar.gz
```

Download URL:

```text
https://www.hessian-matrix.com/wp-content/uploads/2026/automaticupdates/x5_4cam_cross_toolchain_20260708.tar.gz
```

The archive was checked to contain:

```text
cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
cross_compile/new/toolchain/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
cross_compile/new/toolchain/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-strip
X5 SDK platform_samples, sysroot, headers, and libraries
```

The archive does not contain the host `cmake` executable. Its `cmake/` directories are SDK/ROS CMake metadata, not a CMake installation. After extraction, set the toolchain as follows:

```bash
cd <4cam-repo-root>
tar -xzf cross_compile_toolchain/x5_4cam_cross_toolchain_20260708.tar.gz \
  -C cross_compile_toolchain

export X5_TOOLCHAIN_ROOT="$PWD/cross_compile_toolchain/x5_4cam_cross_toolchain_20260708"
export TOOLCHAIN_FILE="$X5_TOOLCHAIN_ROOT/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake"
```

Confirm that CMake is installed separately on the host:

```bash
cmake --version
```
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
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_sensor_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_imu_reader_demo.sh
TOOLCHAIN_FILE=/path/to/aarch64_x5_host_toolchain.cmake scripts/build_serial_port_demo.sh
```


Generated binaries:

- `build_x5/sensor_demo`
- `build_x5/imu_reader_demo`
- `build_x5/serial_port_demo`
- `build_x5/cam_demo`

Check the target architecture:

```bash
file build_x5/sensor_demo
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

When integrated in the top-level workspace, `sub_module/RoboBaton_4p_demo/demo/` is the board-side runtime update package; when this repository is read standalone, the same package is this repository's local `demo/` directory. Users can copy the contents of `demo/` directly to `/root/demo/` on X5.

> Current repository status: as of 2026-07-24, `demo/` was regenerated from the current C ABI v2 sources and the three delivered shared libraries. It passed `scripts/verify_runtime_package.py` and `manifest.sha256` verification; AArch64 builds passed for all four demo targets. The final `sensor_demo` board smoke passed with 12,424 valid IMU samples at 1002.63 Hz, zero invalid/duplicate/regressed timestamps, and exit code 0. The board restored GPIO395/417 and SPI ownership after exit.
>
> The frozen `CLOCK_REALTIME-CLOCK_MONOTONIC_RAW` offset, shared camera/IMU `system_realtime` epoch, and runtime REALTIME-jump immunity added on 2026-07-28 have completed non-ROS T1/T2/T3/T3.1/T4/T5 acceptance. See the top-level `docs/test/FROZEN_SYSTEM_TIMESTAMP_FINAL_ACCEPTANCE_REPORT.md` and reusable `docs/test/FROZEN_SYSTEM_TIMESTAMP_TEST_RUNBOOK.md`.

After code or shared-library changes, maintainers should rebuild the dependent libraries and refresh `demo/` on the development host:

```bash
cd <4cam-repo-root>/sub_module/RoboBaton_4p_demo
scripts/package_runtime.sh
```

`scripts/package_runtime.sh` is the release-repository consumer build and packaging entry point. It reads the producer runtime libraries and public headers already provided in `./lib` and `./include`, configures and builds this repository's four demo targets, then atomically publishes and verifies `./demo`. It does not compile `icm42688_driver.cpp` and does not access or depend on producer source code from the parent workspace.

The runtime package contains the top-level launchers, `env.sh`, `bin/`, and `lib/`. Deploy the complete contents of `demo/` to the board. Do not copy only one executable or one `.so` file.

Deploy it to X5:

```bash
ssh root@<x5-ip> "rm -rf /root/demo && mkdir -p /root/demo"
tar -C demo -cf - . | ssh root@<x5-ip> "tar -xf - -C /root/demo"
ssh root@<x5-ip> "chmod +x /root/demo/cam_demo /root/demo/sensor_demo /root/demo/imu_reader_demo /root/demo/serial_port_demo /root/demo/bin/*"
```

Note: copy the contents of `demo/`, not the outer `demo/` directory itself; the board should not contain `/root/demo/demo/`.

Runtime layout on X5:

```text
/root/demo/
├── cam_demo
├── sensor_demo
├── imu_reader_demo
├── serial_port_demo
├── env.sh
├── bin/
│   ├── cam_demo
│   ├── sensor_demo
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
./sensor_demo
./cam_demo
./imu_reader_demo
./serial_port_demo
```

The top-level `sensor_demo`, `cam_demo`, `imu_reader_demo`, and `serial_port_demo` files are launcher scripts. They set:

```bash
LD_LIBRARY_PATH=/root/demo/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib
```

The real ELF binaries are under `bin/`. To run a `bin/` binary directly, load the environment first:

```bash
cd /root/demo
. ./env.sh
./bin/cam_demo
```

All four demo launchers provide default configurations. For normal bring-up, run `./sensor_demo` for the joint camera/RTSP plus INT1 IMU path, `./cam_demo` for camera/RTSP only, or `./imu_reader_demo` for standalone INT1 IMU. Use command-line options only when changing FPS, bitrate, serial port, sample count, IMU sample rate, or other runtime parameters.

## 4. `sensor_demo`: Joint Camera, RTSP, and IMU Entry Point

Source: `src/sensor_demo.cpp`.

`sensor_demo` starts the four-camera SC132 pipeline, PRRTSP v2, and the independent GPIO395 DRDY + sensor-timestamp FIFO IMU path:

```text
SC132 GPIO417 trigger
  -> frame-set.group_timestamp_ns
  -> PRRTSP v2 timestamp_ns

ICM GPIO395 DRDY rising edge
  -> host edge timestamp anchor
ICM FIFO TMST
  -> sample_timestamp_ns / icm42688 sample callback
```

Both `sensor_demo` and `imu_reader_demo` use `ICM42688_READ_MODE_SENSOR_TIMESTAMP_FIFO`. `sensor_demo --sample-rate-hz <hz>` accepts `25/50/100/200/500/1000/2000` and defaults to `1000`. The IMU path does not use GPIO397, FSYNC, or `icm42688_pulse_fsync()`. Shutdown quiesces the camera/RTSP pipeline before stopping the IMU acquisition thread.

At startup the process prints `TIME_BASE realtime_start_ns=... monotonic_raw_start_ns=... frozen_offset_ns=...`. `system_realtime` outputs are produced by applying this frozen `CLOCK_REALTIME - CLOCK_MONOTONIC_RAW` offset. In `software_gpio`/`gpio` trigger modes, camera diagnostics (`camera_ts_ns`) and RTSP PTS are mapped to that system-time epoch; other trigger modes preserve the SC132 native timestamp domain. IMU `host_timestamp_ns`/`sample_timestamp_ns` are always mapped to `system_realtime`. GPIO395 remains the IMU DRDY edge anchor, and FIFO TMST still defines the per-sample relative timeline.

### `sensor_demo` `[FRAME_SET] trigger_sync` diagnostic log

With `sensor_demo --diagnostics`, the SC132 frame-set matcher may periodically print:

```text
[FRAME_SET] trigger_sync matched_total=6961 discarded_total=28 trigger_seq=6989 lag_ns=9728000 interval_max_lag_ns=9738583 limit_ns=16666666
```

Field definitions:

| Field | Meaning |
|---|---|
| `matched_total` | Cumulative number of successful GPIO trigger/frame-set matches since the current frame-set sync state was reset. |
| `discarded_total` | Cumulative number of stale or no-longer-used trigger queue entries discarded during matching. It is not an RTSP frame-drop counter or a retryable frame-set-drop counter; a nonzero value alone is not a failure. |
| `trigger_seq` | Sequence number of the latest matched GPIO417 software rising edge; it is not a sensor hardware frame ID. |
| `lag_ns` | The earliest frame timestamp in the current frame-set minus the matched GPIO trigger timestamp: `frame_timestamp_ns - trigger_timestamp_ns`. |
| `interval_max_lag_ns` | Maximum lag observed since the previous `trigger_sync` report. It is cleared after printing; the report window is approximately 1 second, so this is not the historical maximum for the entire soak. |
| `limit_ns` | Maximum permitted trigger lag. At the default 60 Hz camera rate it is `16666666 ns`, approximately one frame period. |

The example means:

```text
current lag       = 9.728000 ms
interval max lag  = 9.738583 ms
allowed limit     = 16.666666 ms
current margin    = 6.938666 ms
```

In this example:

```text
matched_total + discarded_total = trigger_seq
6961 + 28 = 6989
```

This line therefore indicates that the matcher is still progressing and the lag is below one frame period. For anomaly diagnosis, also inspect:

```text
trigger_retryable
no valid GPIO417 trigger timestamp
worker fatal
four camera last_seq/fps
four RTSP frame counts
queue_full_rejects
```

`trigger_sync` is a diagnostic progress line, not an independent PASS/FAIL result. Continue with the T3/T4/T5 runbook only when it accompanies a stopped frame-set producer, all four RTSP streams stopping, a retryable burst limit, a structural fatal, or a timestamp mismatch.

Run it from the complete package:

```bash
cd /root/demo
./sensor_demo
```

```bash
./sensor_demo --sample-rate-hz 2000
```

On exit it prints an IMU summary such as:

```text
SENSOR_IMU_RESULT samples=... invalid=... timestamp_duplicates=... timestamp_regressions=... effective_hz=...
```

The joint entry keeps the existing `libprrtsp.so.2` and PRRTSP v2 ABI; it does not add a new PRRTSP API or SONAME.

### 4.1 SC132 4-Camera RTSP Demo

`cam_demo` demonstrates how to use:

- `libsc132.so`: starts the SC132 4-camera pipeline and provides synchronized NV12 DMA frames through a frame-set callback
- `libprrtsp.so`: sends the four NV12 streams to the X5 encoder and publishes RTSP streams

The four demo executables are linked for the X5 runtime environment. Keep `sensor_demo`, `cam_demo`, `include/`, and the libraries under `lib/` from the same package version. Do not mix same-named `.so` files from system directories or other projects, or startup/runtime symbol mismatches may occur.

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

`--trigger-mode` defaults to `software_gpio`, matching the delivered 4-camera external trigger wiring. For normal use, run `./cam_demo` directly for fixed four-camera, 60fps, H.264, upright `1280x1088` output; run `./cam_demo --codec h265` to switch all four streams to H.265.

Deploy the complete `/root/demo` runtime package. The top-level launchers set `LD_LIBRARY_PATH`; if only `bin/cam_demo` or a single `.so` is copied, the board may load system libraries instead of the package libraries.

Common options:

```text
--width <pixels>   Frame width, default 1280
--height <pixels>  Frame height, default 1088
--fps <25|30|40|50|60> Camera and encoder fps, default 60
--codec <h264|h265> Video codec, default h264
--rotate <0|90|180|270> Output rotation, default 0; 180 is limited to 30fps and is not supported at 25/40/50/60fps
--bps <kbps>       Target average encoder bitrate in kbps, default 4000; override it for the required bandwidth/quality trade-off
--url <path>       RTSP path, default /PRR
--trigger-mode <software_gpio|vin_lpwm|none> Trigger output mode, default software_gpio/GPIO417
--diagnostics      Print per-channel send timing and timestamp skew diagnostics
--max-skew-ns <ns> Frame-set timestamp skew release limit, default 2000000; after synchronized grouping, all four exposed frame_id values match exactly
--frame-timeout-ms <ms> Timeout for waiting for missing channels in a frame set, default 100
```

Limit: default `./cam_demo` uses fixed four-camera, 60fps, H.264, upright `1280x1088` output. `--fps` supports `25/30/40/50/60`; `--codec h265` uses the same four ports and paths. `--rotate 180` is supported only in the reduced-load 30fps mode and is not supported at 25/40/50/60fps.

### H.265 Client Playback Notes

The board-side encoder and RTSP interface for `--codec h265` are complete and can publish four fixed H.265 streams. When four `1280x1088@60fps` streams are displayed concurrently, some clients may stutter because their H.265 receive, software-decode, or render throughput is insufficient. This does not by itself indicate a board-side encoder or RTSP transmission failure.

Check both sides when diagnosing playback:

- If the board reports per-channel `fps` close to the target, keeps `queue_full_rejects=0`, and `ffprobe`/`ffmpeg` continuously receives the `hevc` streams, the bottleneck is more likely in the client buffer, decoder, or display path.
- Prefer a player with H.265 hardware decoding and verify that hardware decoding is actually active. Older players or software-only decoding may not sustain four 60fps streams.
- If the client still cannot play in real time, reduce `--fps` to a lower `25/30/40/50` mode, display fewer channels concurrently, or lower the output resolution. Reducing `--bps` mainly reduces network bandwidth and generally does not reduce decode/render load by the same ratio.
- With the same `--bps`, H.264 and H.265 have approximately the same target average bitrate and network bandwidth. H.265 enables a lower target bitrate at comparable quality; it does not automatically reduce bandwidth when both codecs use the same bitrate target. Actual bandwidth also depends on rate control, GOP/I-frame peaks, and RTP/RTSP/TCP/IP overhead, so measure per-stream `bytes/s`.

H.265 acceptance must therefore verify both that the board continuously publishes a valid bitstream and that the target client can decode and render it in real time. Do not use one player's visual smoothness as the sole indicator of board-side interface health.

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
# Default ./cam_demo
codec_name=h264

# ./cam_demo --codec h265
codec_name=hevc

width=1280
height=1088
avg_frame_rate=60/1
```

Diagnosis guide:

- If the board log prints `Found sensor_name:sc132gs-1280p` and `ffprobe` receives the selected `h264` or `hevc` stream, that sensor plus its I2C, MIPI/VIN, and RTSP path are basically healthy.
- If only one `--camera-id` fails, check that camera connector, FPC cable, power, and cable orientation first.
- If all four sensors work individually but the default four-camera mode fails, check the four-camera trigger wiring, GPIO417 external trigger, `cam-service`, and whether another camera process is using the hardware.

Single-sensor diagnosis supports only `--camera-id 0/1/2/3`; do not use this mode to validate two-camera or three-camera combinations.

Frame flow:

1. `cam_demo` registers the synchronized four-camera callback through the `libsc132.so` frame-set API.
2. `libsc132.so` synchronizes the four camera frames and emits a frame-set callback after grouping succeeds.
3. The demo calls the user hook inside the frame-set callback, then retains each frame and pushes it into the corresponding RTSP queue.
4. If a queue is full, the callback neither waits nor overwrites older frames; it rejects the current frame and fails the entire pipeline closed.
5. Worker threads pop frames, build `prrtsp_nv12_frame_v2`, and call `prrtsp_stream_send()`.
6. Worker threads call `sc132_frame_release()` after processing.

The user development hook for synchronized four-camera data is `OnSynchronizedFrameSet()` in `src/cam_demo.cpp`. The callback receives four frames under one `group_id`, with `max_skew_ns`, per-camera `camera_id`, `sequence`, `frame_id`, and `timestamp_ns`. `libsc132.so` releases a group only when normalized `frame_id` values match and timestamp skew stays within the configured limit. The default `2000000 ns` covers the measured approximately `1.06 ms` same-frame pipeline phase at 30 fps while remaining far below one frame period. Do not keep raw frame pointers beyond the callback lifetime unless you call `sc132_frame_retain()` and later call `sc132_frame_release()`.

Log fields:

- `seq`: per-camera software sequence
- `group_id`: synchronized frame-set sequence generated by `libsc132.so`
- `group_skew_ns`: maximum timestamp skew within the frame set, in `ns`, used to diagnose pipeline phase offset
- `frame_id`: synchronized frame-set id; all four frames under the same `group_id` must expose exactly the same value
- `camera_ts_ns`: camera frame timestamp in `ns`; in the default `software_gpio/GPIO417` mode it is the matched GPIO trigger timestamp mapped into the `system_realtime` epoch by the frozen offset; in other trigger modes, the sensor/VIO per-frame timestamp is preferred and the system output timestamp is the fallback
- `queue_full_rejects`: cumulative frames rejected because a per-channel queue was already full; this must remain `0` during stable streaming, and any nonzero value triggers fail-closed shutdown
- `pipeline_delay_ms`: time from enqueue to RTSP send completion
- `send_avg_ms` / `send_max_ms`: `prrtsp_stream_send()` call timing when `--diagnostics` is enabled
- `rtsp_latest_skew_ms`: timestamp skew across the latest sent frames when `--diagnostics` is enabled

### ICM ABI v2 Boundary

The ICM delivery keeps ABI major 2, `libicm42688.so.2`, and `ICM42688_X5_2.0`. The only supported mode is `ICM42688_READ_MODE_SENSOR_TIMESTAMP_FIFO=0` with watermark 1 and a documented ODR. Legacy `DIRECT=1`, the old `FIFO` enum name, the direct-register path, and watermark 8 are retired without a compatibility shim. Keeping v2 means the exported functions, struct layout, and ELF identity remain stable; it does not mean legacy DIRECT configurations remain drop-in compatible. Ship the current header, SO, and non-ROS demos as one generation. ROS2 is outside this change.

See the top-level decision `docs/decisions/2026-07-28-icm-v2-sensor-timestamp-fifo-only.md`.

## 5. IMU Reader Demo

Default run:

```bash
./imu_reader_demo
```

Example:

```bash
./imu_reader_demo --sample-rate-hz 2000 --count 10000
```

Supported IMU sample rates are `25/50/100/200/500/1000/2000 Hz`; the default remains `1000 Hz`.

Terminal output defaults to `10 Hz` while the program still consumes and counts every
IMU sample. Set `--print-rate-hz` explicitly to change the output rate; it must not
exceed `--sample-rate-hz`. Set it to `0` to disable terminal output without changing
`--count` semantics.

Output fields:

- `ts_ns`: IMU sample timestamp in the `system_realtime` epoch, in `ns`; it is derived from the `CLOCK_MONOTONIC_RAW` FIFO TMST timeline plus the startup frozen offset
- `host_ts_ns`: GPIO395 DRDY edge anchor mapped to the `system_realtime` epoch, in `ns`
- `dt_ms`: timestamp delta between adjacent printed `ts_ns` samples in `ms`; it is typically about `100 ms` at the default 10 Hz output, or `1000 / --print-rate-hz` ms with a non-zero explicit print rate; `--print-rate-hz 0` produces no per-frame `dt_ms` output
- `temp_c`: temperature in `degC`
- `accel_mps2`: 3-axis acceleration in `m/s^2`
- `accel_norm_mps2`: acceleration norm, typically close to `9.81` when stationary
- `gyro_rps`: 3-axis angular velocity in `rad/s`

Notes:

- The demo uses GPIO395 DRDY + sensor-timestamp FIFO mode (`ICM42688_READ_MODE_SENSOR_TIMESTAMP_FIFO`).
- `host_timestamp_ns` records the GPIO395 rising-edge anchor; `sample_timestamp_ns` is the per-sample timestamp mapped from FIFO TMST. Before the demo prints or forwards them, the same `TIME_BASE` frozen offset converts both values to `system_realtime`.
- The IMU path does not use GPIO397, FSYNC, or `icm42688_pulse_fsync()`.
- The driver callback runs on the acquisition thread and only enqueues into the bounded 64-slot FIFO; custom observers and CLI output run on the owner thread.
- CLI output uses one non-blocking write per line. If an SSH session, pipe, or log collector slows down or closes, CLI log lines are dropped while the owner continues consuming every IMU sample.
- The 10 Hz default further reduces normal terminal traffic. Increase `--print-rate-hz` explicitly for diagnostics, but log completeness is not guaranteed with a slow sink.
- If a custom observer or other owner-side computation remains slower than the sample period, the bounded 64-slot FIFO still fails closed rather than silently dropping IMU samples.

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

After deployment, first confirm all four demos can print their help text:

```bash
cd /root/demo
./cam_demo --help
./imu_reader_demo --help
./serial_port_demo --help
./sensor_demo --help
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
- The log keeps `queue_full_rejects` at `0`.
- No obvious error, crash, or repeated camera restart appears.

The complete 30 fps regression script is not part of the `/root/demo` runtime package. It is an SSH-driven tool in the development-host source tree. First deploy the complete `demo/` directory to `/root/demo` on the board, then run this from the `4cam` repository root on the development host:

```bash
cd <4cam-repo-root>
sub_module/RoboBaton_4p_demo/scripts/cam_demo_regression.sh \
  --host <x5-ip> \
  --fps 30 \
  --min-fps 28 \
  --max-group-skew-ns 2000000 \
  --kill-existing
```

Do not run `scripts/cam_demo_regression.sh` from `/root/demo` on the board. The runtime package contains only `bin/`, `lib/`, the top-level launchers `cam_demo`, `imu_reader_demo`, and `serial_port_demo`, plus `env.sh` and `manifest.sha256`.

## 8. Runtime Constraints

The IMU demo uses the current X5 mainboard connection by default:

- SPI device node: `/dev/spidev2.0`
- SPI mode: `0`
- SPI speed: `4 MHz`
- Default read mode: sensor-timestamp FIFO

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
./bin/imu_reader_demo --sample-rate-hz 2000 --count 10
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

- If `fps` is close to 60 and `queue_full_rejects=0`, but one player view is visibly slower, the delay is more likely in the RTSP client buffer or display path.
- If `send_max_ms` stays unusually high, continue checking that RTSP or encoder path.
- If `group_skew_ns` stays close to one frame period, continue checking external trigger stability, camera startup order, and board load.
