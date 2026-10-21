# X5 SC132 4-Camera, IMU, UART, and CAN Demo

中文说明：[README.md](README.md)

![RoboBaton 4P](image/4P_Cam.png)

This is the public V2 non-ROS demo repository for RoboBaton 4P. It provides the X5 runtime package, SC132 four-camera RTSP examples, ICM-42670-P IMU examples, UART1/UART7 examples, a SocketCAN CAN example, public headers, and matching prebuilt runtime libraries.

> **The [4P_doc](https://4p-doc-en.readthedocs.io/en/latest/index.html) documentation is authoritative for end users.** This README keeps only the repository entry point, minimum run path, and support boundaries. Use the documentation site for deployment, persistence, wiring, data/API contracts, and troubleshooting.

Current source uses `icm42670_*` / `icm42670_driver.h`, SONAME `libicm42670.so.2`, and real SO `libicm42670.so.2.0.0`. IMU ABI 2.1 retains base functions in `ICM42670_X5_2.0` and adds the FSYNC callback in `ICM42670_X5_2.1`. Old V1 downloads do not implement this interface. Software versions, documentation revisions, and board acceptance are independent; an identical version string does not prove an identical manifest. The image above is a V1 illustration, not V2 pinout or photographic evidence.

## Contents

```text
demo/       Complete X5 package for `/root/demo` (includes the `mosaic_rtsp_demo` four-view example)
include/    Public C headers
lib/        Prebuilt libraries matching the current demo and headers
config/     Default `sensor_demo` YAML
src/        Demo source (includes `mosaic_rtsp_demo.cpp` / `mosaic_nv12.*`)
scripts/    Build, packaging, and runtime verification entry points (includes `build_mosaic_rtsp_demo.sh`)
```

Users normally need only `demo/`. Do not replace only one ELF, one `.so`, or the configuration file.

Camera ABI 2.2 provides a frozen per-frame ISP exposure observation in `sc132_frame_info_t.exposure`, also saved in bag frame metadata, MP4 `exposure.jsonl`, and tee indexes. `status=1` is an estimate, not certified applied exposure or an actual exposure midpoint; `status=0` is unavailable. See [Data Contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html) for fields, join keys, and limits.

## Version and package matching

Use the repository, `demo/`, prebuilt libraries, and public documentation as one release composition. Confirm `VERSION`, the repository tag, and `demo/manifest.sha256`; do not mix source, headers, shared libraries, and runtime packages from different releases.

On the board, query program and runtime-library versions:

```bash
cd /root/demo
./cam_demo --version
./mosaic_rtsp_demo --version
./sensor_demo --version
./imu_reader_demo --version
./serial_port_demo --version
./can_demo --version
```

Each demo's `--version` prints its product version and the project-owned shared objects linked by that process: `cam_demo` and `mosaic_rtsp_demo` report `libsc132`/`libprrtsp`, `sensor_demo` reports `libicm42670`/`libsc132`/`libprrtsp`, and `imu_reader_demo` reports `libicm42670`. This detects mixed packages.

These version queries do not initialize the camera, IMU, or UART. For compatibility, ABI, and release details, see [Product and compatibility](https://4p-doc-en.readthedocs.io/en/latest/getting-started/product-and-compatibility.html), [API reference](https://4p-doc-en.readthedocs.io/en/latest/development/api-reference.html), and [Changelog](https://4p-doc-en.readthedocs.io/en/latest/changelog.html).

## Choose a demo

| Program | Purpose |
|---|---|
| `sensor_demo` | Four cameras, RTSP, and IMU together; optional ROS1 bag or H.264 MP4 persistence |
| `cam_demo` | Four cameras and RTSP |
| `mosaic_rtsp_demo` | Four NV12 views combined into one RTSP stream, default port `558` |
| `imu_reader_demo` | Standalone IMU reading |
| `serial_port_demo` | UART1/UART7 example; not for DEBUG_UART |
| `can_demo` | TCAN4550 SocketCAN classical CAN / CAN FD transmit/receive example, explicit FD/BRS opt-in; preconfigured interface required |

## Default run

The default package is `demo/`. After deployment, run on X5:

```bash
cd /root/demo
./sensor_demo
```

Camera/RTSP only:

```bash
./cam_demo
```

Run `./mosaic_rtsp_demo` for four-view RTSP at `rtsp://<x5-ip>:558/PRR`; `--fps` supports `25/30/40/50/60`. See [Mosaic usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html#non-ros-mosaic) for parameters and limits.

IMU only:

```bash
./imu_reader_demo
```

After stop, `imu_reader_demo` emits one `IMU_RUNTIME_HEALTH final=yes` record with session validity, producer/consumer sample counts, output drops, and final GPIO/FIFO/mapper/invalid-packet counters. Samples and the final record use best-effort nonblocking output; a missing final record is not proof of healthy completion. `--print-rate-hz 0` disables samples, not the final record.

UART1/UART7 example:

```bash
./serial_port_demo
```

### CAN Demo

The BSP must register and configure a CAN network interface first. `can_demo` does not access SPI registers or change device tree, GPIO, bitrate, or interface state. By default it transmits classical CAN standard/extended data frames and receives data/remote frames (`0..8` bytes). `--fd` enables FD transmission or mixed classical/FD reception and requires an FD-enabled interface (MTU `72`); `--brs` enables the data-phase bit-rate switch only with `--mode tx --fd`. FD transmit lengths are `0..8,12,16,20,24,32,48,64` bytes; other lengths are rejected without implicit padding. FD has no remote frames.

```bash
cd /root/demo
./can_demo --help
# Receive on can0; Ctrl-C stops. Alternatively set a finite count and idle timeout.
./can_demo --interface can0 --mode rx --count 10 --timeout-ms 5000
# Send 10 standard frames; hexadecimal ID, payload without separators.
./can_demo --interface can0 --mode tx --id 123 --data 0102030405060708 --count 10 --interval-ms 100
# 29-bit extended frame; sends once by default.
./can_demo --interface can0 --mode tx --extended --id 1ABCDE --data AABB
# After configuring FD with the peer's arbitration/data bitrates, receive mixed classical/FD frames.
./can_demo --interface can0 --mode rx --fd --count 10 --timeout-ms 5000
# 16-byte FD frame; omit --brs to keep the arbitration bitrate during the data phase.
./can_demo --interface can0 --mode tx --fd --brs --id 123 --data 000102030405060708090A0B0C0D0E0F
```

Without options, receive indefinitely. TX defaults to one `123#0102030405060708` frame; `--count 0` runs continuously. Exit codes: `0` completed or interrupted normally, `1` interface/I/O/receive-timeout failure, `2` invalid arguments. `tx_queued` means kernel acceptance only; local SocketCAN loopback is not external-bus proof. See [CAN usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html#non-ros-can) for interface setup, wiring, and two-node checks. Build alone with `TOOLCHAIN_FILE="$TOOLCHAIN_FILE" scripts/build_can_demo.sh`; use `scripts/package_runtime.sh` to regenerate the full `demo/` package.

FD output is `ID##<flags><data>`, where flags bit 0 is BRS and bit 1 is ESI, e.g. `tx 123##1000102030405060708090A0B0C0D0E0F`. Classical output is unchanged; `--extended` selects only a 29-bit ID, not FD. Application FD support is not board-level BSP integrity acceptance. Long FD payloads may be corrupted on affected BSPs; fix the underlying issue and verify the full payload byte-for-byte at a second physical node.

For first power-on, network setup, deployment prerequisites, and the required `cam-service`, see [First power-on](https://4p-doc-en.readthedocs.io/en/latest/getting-started/first-boot.html) and [Quick start](https://4p-doc-en.readthedocs.io/en/latest/quick-start.html).

### Start sensor_demo at boot

Boot autostart of `sensor_demo` is managed by the top-level `start_sensor_demo.sh`:

```bash
cd /root/demo
./start_sensor_demo.sh            # run sensor_demo in the foreground (same as ./sensor_demo)
./start_sensor_demo.sh enable     # install boot autostart (requires root)
./start_sensor_demo.sh disable    # remove boot autostart (requires root)
./start_sensor_demo.sh status     # show autostart and process state
```

Autostart is implemented through `/userdata/startup.sh`, executed by the system `S99auto_startup` after `/userdata` is mounted. The script only appends/removes its own marked block and never overwrites content written by other scripts such as `wifi_setup.sh`. Autostart uses the packaged default configuration; edit `config/sensor_config.yaml` to adjust it, no need to re-run `enable`.

For the exposure-midpoint test, set `test_exposure_midpoint: true` in the `camera` section of `config/sensor_config.yaml`; default `false`. Restart `sensor_demo` to apply it. Explicit YAML overrides `SC132_TEST_EXPOSURE_MIDPOINT`; only an omitted key preserves environment control. Image timestamps, including ROS1 bag image `header.stamp`, use trigger time plus half exposure duration; IMU stamps and bag record time are unchanged. Unavailable exposure stops acquisition. Rebuild both the application and a supporting `libsc132.so`; old prebuilt packages do not gain support automatically. This is not a calibrated actual midpoint. See [Data Contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html#test-exposure-midpoint).

## Persistence

`sensor_demo` does not persist data automatically. The default format field is MP4 and the default path is `/root/demo/save_mp4/`. Explicitly select one mode when enabling persistence:

```bash
# ROS1 bag v2.0
./sensor_demo --record-bag /data/run.bag

# H.264 MP4 session
./sensor_demo --record-mp4-dir /data/mp4_session
```

ROS1 bag and MP4 are mutually exclusive. MP4 supports H.264 and the complete four-camera mask; frame skip applies only to ROS1 bag. For output files, stop order, completeness rules, partial/recovery handling, and offline extraction, see [Data persistence](https://4p-doc-en.readthedocs.io/en/latest/usage/save-data-guide.html).

Current MP4 sessions always create `exposure.jsonl` and `fsync.jsonl`; the latter has records only when FSYNC is enabled. Exposure sidecars describe input observations and may outnumber saved encoded frames. Join by camera and microsecond-quantized group-trigger time against CSV, not by row number or source frame ID as an MP4 index. Default `--imu-sample-drop-policy allow-counted` can continue with counted sample losses. Bag complete does not independently prove zero producer drops; MP4 complete additionally checks final producer health and sample counts. A complete dataset requires actual payload readback, not just an exit code or playable MP4s.

## Build from source

Cross-compile on an Ubuntu 22.04 x86_64 host and run the generated package on X5. Provide matching `./include`, `./lib`, the `x5_4cam_cross_toolchain_20261009` bundle, and host CMake. See the build page below for download availability and host installation steps.

```bash
export X5_TOOLCHAIN_ROOT="/path/to/x5_4cam_cross_toolchain_20261009"
source "$X5_TOOLCHAIN_ROOT/setup_4cam_cross_env.sh"
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
cmake --build build_x5 --parallel 4
```

Maintainers refresh the complete runtime package with:

```bash
scripts/package_runtime.sh --toolchain-file "$TOOLCHAIN_FILE"
python3 scripts/verify_runtime_package.py demo
```

Packaging supports standalone clones and verifies the source commit, file hashes, and complete runtime package. A custom `--build-dir` must stay inside the source repository; use a new build directory after switching bundles.

For toolchain preparation, dependencies, build boundaries, and package verification, see [Open-source demo build](https://4p-doc-en.readthedocs.io/en/latest/development/open-source-build.html).

## Deployment

The board target directory is `/root/demo`. Deployment must follow:

```text
unique temporary directory
→ complete manifest verification
→ old-application exit check
→ backup of the old directory
→ atomic promotion
→ help/smoke verification
→ restore the latest backup on failure
```

Do not delete `/root/demo` before uploading a new package, and do not test the camera with `cam-service` stopped. For the complete procedure, see [Deployment, upgrade, and rollback](https://4p-doc-en.readthedocs.io/en/latest/development/deployment-and-upgrade.html).

## Support-boundary summary

- Camera output is NV12, `1280x1088` by default, with optional VSE hardware full-frame scaled canvases `640x480`, `720x480`, and `1280x720`; RTSP defaults to H.264, with optional H.265, path `/PRR`, and default ports `554..557`.
- The public camera set is `25/30/40/50/60fps`, default `30fps`; `rotate=180` is supported only at `30fps`.
- The IMU natively supports `25/50/100/200/400/800/1600Hz`, default `800Hz`, using sensor-timestamp FIFO. Old `500/1000/2000Hz` values are explicitly rejected, not silently resampled.
- All camera entries use VIN LPWM hardware triggering. Camera and IMU FSYNC share the LPWM IO; INT1 uses `34120000.gpio` offset `8`. Camera hardware trigger edges, IMU samples, and INT1 times map to `CLOCK_REALTIME`, retained unchanged by CLI/saving; bag record time independently reads realtime. Bridges refresh during operation and follow backward clock steps; internal timeouts remain monotonic. Cameras require read-only X5 `/dev/mem` access and stop on missing captures/bounded mappings without arrival-time fallbacks. A common domain does not provide FSYNC sample/exposure associations, TF, or calibration.
- `sensor_demo --fsync --sample-rate-hz 1600` or YAML `imu.fsync_enabled: true` enables trigger-to-sample association (default off, only `800/1600Hz`). Results reach stdout, bag `/robobaton/fsync`, or MP4 `fsync.jsonl`. Accept only `status=1`; missing/ambiguous cases are explicit. This changes neither sample phase nor exposure-sync or zero-loss guarantees.
- The IMU consumer uses `SCHED_RR/3`, the acquisition owner uses `SCHED_FIFO/20`, the camera RAW observer uses `SCHED_FIFO/60`, and the independent INT1 clock observer uses `SCHED_FIFO/30`; both observers inherit the allowed CPU mask and sleep `50µs` per iteration. Realtime scheduling permission is required. The INT1 observer stays below the board's default FIFO/50 GPIO IRQ thread and retains edge-time certificates through SPI waits and callback delays. Cross-priority queues use priority-inheritance locks, but applications do not automatically change system IRQ/debug settings. Stress acceptance must inspect final post-stop `IMU_RUNTIME_HEALTH` and read actual saved payloads; counted drops are not zero loss.
- UART1/UART7 retain `3.3V`, DEBUG_UART retains `1.8V`. The V2 UART7 5-pin connector has an independent PPS input enabled by default, without RX/TX multiplexing. Use official V2 pinout, power limits, and hot-plug boundaries, not the V1 `500mA` limit or historical acceptance.
- Camera operation requires the X5 board-side `cam-service`; this is not a general-purpose host package.

For complete camera, IMU, UART, timestamp, frame-rate, and data semantics, see [Data contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html), [non-ROS Demo usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html), and [Hardware and safety](https://4p-doc-en.readthedocs.io/en/latest/getting-started/hardware-and-safety.html).

## Troubleshooting

When startup, shared-library, RTSP, IMU, or UART problems occur, retain `VERSION`, manifest results, the command, exit code, and necessary logs. Do not submit real IPs, credentials, or internal paths. Start with [Troubleshooting](https://4p-doc-en.readthedocs.io/en/latest/troubleshooting.html).

## Public documentation index

- [Product introduction](https://4p-doc-en.readthedocs.io/en/latest/getting-started/Product_Introduction.html)
- [Product and compatibility](https://4p-doc-en.readthedocs.io/en/latest/getting-started/product-and-compatibility.html)
- [First power-on](https://4p-doc-en.readthedocs.io/en/latest/getting-started/first-boot.html)
- [Quick start](https://4p-doc-en.readthedocs.io/en/latest/quick-start.html)
- [non-ROS Demo usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html)
- [Deployment, upgrade, and rollback](https://4p-doc-en.readthedocs.io/en/latest/development/deployment-and-upgrade.html)
- [Open-source demo build](https://4p-doc-en.readthedocs.io/en/latest/development/open-source-build.html)
- [Data persistence](https://4p-doc-en.readthedocs.io/en/latest/usage/save-data-guide.html)
- [Data contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html)
- [API reference](https://4p-doc-en.readthedocs.io/en/latest/development/api-reference.html)
- [Hardware and safety](https://4p-doc-en.readthedocs.io/en/latest/getting-started/hardware-and-safety.html)
- [Troubleshooting](https://4p-doc-en.readthedocs.io/en/latest/troubleshooting.html)
- [Changelog](https://4p-doc-en.readthedocs.io/en/latest/changelog.html)

### Mosaic RTSP Demo

Four-view mosaic RTSP example:

```bash
./mosaic_rtsp_demo
./mosaic_rtsp_demo --fps 40
./mosaic_rtsp_demo --fps 50
```

Fixed RTSP URL `rtsp://<x5-ip>:558/PRR`; supports `--fps <25|30|40|50|60>`. For detailed parameters, run statistics, and boundaries, see [non-ROS Demo usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html#non-ros-mosaic).

License and third-party component information are defined by this repository's `LICENSE` and release documentation.
