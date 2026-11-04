# X5 SC132 4-Camera, IMU, UART, and CAN Demo

中文说明：[README.md](README.md)

This is the public V2 non-ROS demo repository for RoboBaton 4P. It provides the X5 runtime package, SC132 four-camera RTSP examples, ICM-42670-P IMU examples, UART1/UART7 examples, a SocketCAN CAN example, public headers, and matching prebuilt runtime libraries.

> **The [4P_doc](https://4p-doc-en.readthedocs.io/en/latest/index.html) documentation is authoritative for end users.** This README keeps only the repository entry point, minimum run path, and support boundaries. Use the documentation site for deployment, persistence, wiring, data/API contracts, and troubleshooting.

Current IMU capture uses kernel IIO public ABI 5.0: real file `libicm42670.so.5.0.0`, SONAME `libicm42670.so.5`, and nine exports under `ICM42670_X5_5.0`. `host` retains the `CLOCK_MONOTONIC` host estimate; `tmst` additionally maps sensor TMST to `CLOCK_MONOTONIC_RAW`. Both preserve native host time and all burst evidence. Saved output explicitly bridges the selected native clock to realtime per burst, retaining the bridge bracket half-width. A matching kernel must already bind the device. No userspace SPI, FSYNC, or automatic fallback is provided. Rebuild every consumer with matching headers/libraries, without cross-major links. Software, documentation, ABI, and release status remain independent.

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

Each library family in `lib/` and `demo/lib/` has one real `libNAME.so.M.m.p` file and exact same-directory relative links: `libNAME.so.M -> libNAME.so.M.m.p`, `libNAME.so -> libNAME.so.M` (NAME is `icm42670`, `sc132`, or `prrtsp`). Copy complete packages with link-preserving `tar`, `cp -a`, or `rsync -a`, not dereferencing copies. Packaging and verification reject flattened aliases and absolute, wrong, or broken links. `manifest.sha256` checks resolved bytes; v3 `runtime-provenance.json` also binds link text. Regenerate packages with older provenance.

| Library | Real file | SONAME | Symbol nodes |
|---|---|---|---|
| ICM42670 | `libicm42670.so.5.0.0` | `libicm42670.so.5` | `ICM42670_X5_5.0` |
| SC132 | `libsc132.so.3.0.0` | `libsc132.so.3` | `LIBSC132_3.0` |
| PRRTSP | `libprrtsp.so.2.0.1` | `libprrtsp.so.2` | `LIBPRRTSP_2.0` |

`M.m.p` identifies the real-library revision; `M` is the ABI major, independent of software/PCB versions. ICM42670 ABI 5.0 and SC132 ABI 3.0 require matching headers and rebuilt consumers, with old-major libraries removed and no cross-major links or old symbol aliases. Use `sample_sequence` for order; the MONOTONIC host estimate, TMST mapping, and RAW burst observation are not calibrated physical sampling edges.


Camera ABI 3.0 provides a frozen per-frame ISP exposure observation in `sc132_frame_info_t.exposure`, also saved in bag frame metadata, MP4 `exposure.jsonl`, and tee indexes. `status=1` is an estimate, not certified applied exposure or an actual exposure midpoint; `status=0` is unavailable. See [Data Contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html) for fields, join keys, and limits.

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

Accepted settings are not sustained-throughput promises for every saving, streaming, DDS, and load combination. Container completeness, source integrity, and actual-payload completeness are separate; historical compositions cannot certify the current IIO composition. See [Release and support](https://4p-doc-en.readthedocs.io/en/latest/release-and-support.html).

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

`imu_reader_demo --count 800 --print-metrics` requests STOP at the threshold and drains the complete cohort, so final counts may exceed800. `IMU_TIMING_SUMMARY` separates `timestamp_hz` from ordinary-burst RAW-endpoint `acquisition_hz/acquisition_valid`. Post-stop `IMU_RUNTIME_HEALTH final=yes` reports generation, published/observed, scans, records/ACK, fatal, overflow, invalid payload, push/evidence, SERVICE, tail, and quiesce/transport_complete. Failed stop still retains terminal diagnostics. Samples/final records use best-effort nonblocking logging; a missing summary is not a health pass. `--print-rate-hz 0` disables samples only.

UART1/UART7 example:

```bash
./serial_port_demo
```

While waiting for RX, `SIGINT`/`SIGTERM` exits with status `0` and restores the original serial-port settings. Unrelated signal interruptions preserve the RX deadline; genuine device/I/O errors remain failures. External wire transfers and hardware TX backpressure require separate validation.

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

Autostart is implemented through `/userdata/startup.sh`, executed by the system `S99auto_startup` after `/userdata` is mounted. The script only appends/removes its own marked block and never overwrites other applications' content. The image-provided Wi-Fi command `wifi_setup` uses separate `/etc/init.d/S99wifi_setup` and `/userdata/wifi/autostart.enabled`, not this shared file. Autostart uses the packaged default configuration; edit `config/sensor_config.yaml` to adjust it, no need to re-run `enable`.

Select top-level YAML `camera_timestamp_path: observer|official` (default `observer`) and `imu_timestamp_mode: host|tmst` (default `host`) before startup. CLI `--camera-timestamp-path` and `--imu-timestamp-mode` override YAML. All four combinations share the same kernel IIO capture; none switches to userspace SPI. `imu_reader_demo` also accepts `--imu-timestamp-mode`; camera demos accept `--camera-timestamp-path`. Stop and restart to change selectors; invalid values fail before hardware startup.

Camera consumers explicitly set `SC132_TIMESTAMP_PATH` before startup; direct library users default to `observer` when it is unset. `observer` retains the bounded SIF MMIO observer. `official` estimates from same-frame `timestamps - (tv - trig_tv)` without starting that observer; physical uncertainty stays unknown (`UINT32_MAX`) and bounded timing validity is not granted. Both paths output realtime without fallback to the other path; FSYNC remains disabled/unsupported. Example: `./sensor_demo --camera-timestamp-path official --imu-timestamp-mode tmst`.

Bag `/robobaton/session_config` records both selectors. IMU timing JSON and MP4 CSV retain `timestamp_mode`, `fifo_tmst`, `sample_raw_ns`, `timestamp_uncertainty_ns`, and correct bridge clock labels. Host RAW sample time is zero with unknown uncertainty; TMST never overwrites native host/burst evidence.

For repeatable calibration with manual exposure, set these fields in the `camera` section of `config/sensor_config.yaml`, then restart `sensor_demo`:

```yaml
camera:
  auto_exposure: false
  exposure_time_us: 2000
```

`camera.auto_exposure` is a boolean, default `true`: it ignores the exposure duration and preserves the SDK tuning's automatic-exposure policy. `camera.exposure_time_us` defaults to `2000` and must be an integer in `1..4294967295` microseconds; zero, negatives, fractions, overflow, and trailing text are rejected. Manual mode selects ISP `HBN_ISP_MODE_MANUAL` for each enabled camera, converts `exposure_time_us / 1000000` to SDK seconds, and fixes analog, sensor digital, and ISP gains to `1x`. The SDK/profile limits the usable duration; configuration failure fails startup without silently falling back to auto.

The library reads `SC132_AUTO_EXPOSURE=1|0` and decimal `SC132_EXPOSURE_TIME_US` at each frame-set startup. `sensor_demo` always overrides inherited environment values with explicit or default YAML options. Settings are startup-only: stop and restart after changes, and rebuild both the application and a supporting `libsc132.so`. This is independent of the midpoint test below, changes neither timestamp formulas nor the C ABI, and certifies neither applied-frame identity nor a physical exposure midpoint. No board deployment or verification has been performed for this feature.

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

ROS1 bags default to strict drop spacing: `--record-drop-min-interval 10` (YAML: `save_data.drop_min_interval`). Under overload, only fully encoded camera groups not yet claimed by the writer are eviction candidates. Candidates must be at least 10 arrival ordinals from every previous drop; the recorder uses consecutive frame-set arrival order, not possibly sparse producer `group_id`. If no old candidate qualifies, the incoming group may be dropped only under the same rule. Otherwise recording explicitly stops, drains accepted data, publishes `*.partial.bag`, and exits nonzero instead of relaxing the spacing. This does not cover acquisition-source, hardware, or IMU losses and does not guarantee sustained 30 fps throughput. Intentional alternate saving requires `--record-frame-skip 1 --record-drop-min-interval 2` (YAML: `skip: true`, `drop_min_interval: 2`).

Read back `source_group_order` in `/cameraN/frame_metadata` (`N=0..3`) to compare retained groups across cameras; `group_id` preserves producer identity. `/robobaton/session_config` records `drop_policy_min_group_interval`; `/robobaton/session_status` records `drop_policy_stopped`, `drop_policy_stop_group_order`, `drop_policy_dropped_groups`, `drop_policy_min_observed_interval`, and the stop reason. On a policy stop, the terminal ordinal is the exclusive upper bound of recorded coverage, not another legal drop.

MP4 sessions save `exposure.jsonl` and extended `imu.csv`, without an FSYNC sidecar. Bags add `/robobaton/imu/timing` (`std_msgs/String`, `robobaton_imu_timing_v2`). IMU Headers/CSV use per-burst bridged realtime; metadata retains MONOTONIC, RAW, identity, and bridge bases/half-width, not physical precision. Exposure sidecars describe input frames and may outnumber encoded frames; join by camera and microsecond-quantized group time against CSV. `allow-counted` counts only invalid six-axis payloads; MP4 complete also checks final producer health and counts. Read back actual payloads to establish a complete dataset.

## Debug-console autologin switch

On an image with this feature integrated, root uses `uart-autologin status|enable|disable` to manage DEBUG_UART (ttyS0) login. Fresh userdata defaults to passwordless root; disabling restores normal username/password authentication at the next login. The choice persists in `/userdata/uart-autologin/state`, without changing a read-only rootfs, terminating existing sessions, or affecting SSH or UART1/UART7. Anyone with physical debug-console access gains root while enabled; use only in trusted physical environments. Sources are `scripts/env_setup/uart-autologin.sh` and `uart-autologin-login.sh`. Merely copying them into the Demo does not change getty; the image maintainer must integrate the unique console entry.

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
- The IMU natively supports `25/50/100/200/400/800/1600Hz`, API default `800Hz`, using `host` or `tmst` timing over the same kernel IIO capture. Old `500/1000/2000Hz` values are explicitly rejected, not silently resampled.
- Cameras retain VIN LPWM triggering; only the `observer` timestamp path needs read-only X5 `/dev/mem` for its SIF MMIO observer. Invalid selected-path metadata/mapping stops acquisition without switching paths. Combined saving explicitly bridges host MONOTONIC or TMST RAW to realtime. Bag record time reads realtime independently. A common epoch does not establish physical synchronization, TF, or calibration.
- IMU defaults:800Hz, watermark1, buffer4096 (API accepts powers of two in256..8192). STOP keeps callback admission open, drains, and ACKs the full cohort. `transport_complete` certifies only that cohort, not the physical tail beyond its fence or absence of fatal errors. No FSYNC callback, topic, sidecar, or enable option exists.
- The IMU consumer uses `SCHED_RR/3`, the acquisition owner uses `SCHED_FIFO/20`, and the camera RAW observer uses `SCHED_FIFO/60`, inherits the allowed CPU mask, and sleeps `50µs` per iteration. Realtime scheduling permission is required. IMU no longer uses an independent INT1 clock observer or packet/edge identity matching; notification sequence gaps are diagnostic, not independently fatal. Cross-priority queues use priority-inheritance locks. Applications do not automatically change IRQ/debug or realtime quotas, and this redesign does not fix the underlying GPIO/kernel root cause. Inspect final post-stop `IMU_RUNTIME_HEALTH` and actual payloads; counted drops are not zero loss.
- UART1/UART7 use `3.3V`, DEBUG_UART uses `1.8V`. The V2 UART7 5-pin connector has an independent PPS input enabled by default, without RX/TX multiplexing. Use official V2 pinout, power limits, and hot-plug boundaries; confirm pin order and voltage, then wire with power off. Hot-plug support is not promised.
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
