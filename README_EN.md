# RoboBaton 4P non-ROS Demo

[中文](README.md) · [English documentation](https://4p-doc-en.readthedocs.io/en/4pv2.main/)

X5 examples for SC132 four-camera capture, RTSP, ICM-42670-P IMU, UART, and SocketCAN. The V2 main branch is `V2_main`. Wiring, deployment, parameters, recording, APIs, and troubleshooting are maintained in the public documentation repository [4P_doc](https://github.com/Hessian-matrix/4P_doc/tree/V2_main).

## Repository Contents

- `demo/`: complete X5 runtime package with `sensor_demo`, `cam_demo`, `mosaic_rtsp_demo`, `imu_reader_demo`, `serial_port_demo`, and `can_demo`.
- `src/`, `include/`, `lib/`, `config/`: example sources, public headers, matching prebuilt libraries, and default configuration.
- `scripts/`: build, packaging, and runtime verification entry points.
- [`scripts/env_setup/x5_usb/`](scripts/env_setup/x5_usb/): public system USB manager and four-camera UVC sources, standalone build, and offline-image installer.

Current IMU capture uses kernel IIO V4 / public ABI 6.0: real file `libicm42670.so.6.0.0`, SONAME `libicm42670.so.6`, and nine exports under `ICM42670_X5_6.0`. `host` retains the `CLOCK_MONOTONIC` host estimate; `tmst` additionally maps sensor TMST to `CLOCK_MONOTONIC_RAW`. Both preserve native host time and all burst evidence. Saved output explicitly bridges the selected native clock to realtime per burst, retaining the bridge bracket half-width without certifying physical sampling or exposure synchronization accuracy. The matching V4 kernel module must already bind the device; V2/V3 are incompatible even in host mode. Independent FSYNC diagnostics are available; no userspace SPI or automatic fallback is provided. Rebuild every consumer with matching headers/libraries, without cross-major links. Software, documentation, ABI, and release status remain independent.

This repository builds user-side examples, not the underlying runtime libraries. Keep sources, headers, libraries, and configuration matched. Deploy the complete runtime package, preserving symbolic links; do not replace individual `.so` files.

The current `demo/` is the complete ABI6 runtime matching the accepted composition file by file: executables, libraries, configuration, and launcher scripts. Only public provenance and the manifest are refreshed; software remains `2.0.0`. The matching **IIO V4 kernel module** must already bind the device; the application package does not upgrade it. See [Initial Runtime Package and Verified Scope](https://4p-doc-en.readthedocs.io/en/4pv2.main/release-and-support.html#public-runtime-acceptance) for package identity and default-mode board results.

From the complete repository, run:

```bash
python3 scripts/verify_runtime_package.py demo --portable
```

This checks source commits/inputs, payload, symlinks, ELF, and ABI. `--portable` does not require original build-host toolchain files; omit it on the build host to check those files too. SHA-256 of `demo/manifest.sha256`:

```text
ecb2a043e5d52ee4133b4ba9a018ec0af2c1b2ff1a271f3d2e5cab1ff9454b0f
```

## Getting Started

System image download: [4P_V2.0.0_image.tar.gz](https://www.hessian-matrix.com/wp-content/uploads/2026/automaticupdates/4P_V2.0.0_image.tar.gz). Back up data and confirm supported hardware and the publisher's SHA-256 per [System Flashing](https://4p-doc-en.readthedocs.io/en/4pv2.main/ops/system-flashing.html) before flashing; the supplied preinstalled system does not require reflashing.

**The official image download above is unchanged; no image is updated here.** The shared `2.0.0` name does not establish ABI6/IIO V4 contents. Confirm the system module and actual delivery inventory before deploying.

Prepare the device and runtime package using [First Power-On](https://4p-doc-en.readthedocs.io/en/4pv2.main/getting-started/first-boot.html) and [Deployment, Upgrade, and Rollback](https://4p-doc-en.readthedocs.io/en/4pv2.main/development/deployment-and-upgrade.html), then run on X5:

```bash
cd /root/demo
./sensor_demo --version

./sensor_demo
```

Press `Ctrl+C` to stop normally. Keep `cam-service` running and do not run multiple camera applications concurrently. See [non-ROS Demo Usage](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/non-ros-demo.html) for other examples and complete parameters.

The current default is camera `official` + IMU `host`, with IMU at `800Hz`. All four combinations remain: top-level YAML `camera_timestamp_path: observer|official` and `imu_timestamp_mode: host|tmst`, overridden by CLI `--camera-timestamp-path` / `--imu-timestamp-mode`. Stop normally and restart after changing them; there is no hot switching or failure fallback. Default official uses same-frame RAW IRQ `timestamps − (tv − trig_tv) × 1000` (AON microsecond delta converted to ns), never starting the SIF MMIO observer, so scheduling tails exceeding its `500µs` window no longer terminate default capture. Per-group RAW-to-realtime conversion uses the actual measured bridge bracket even beyond observer's `200µs` projection admission, without exiting solely for width or substituting current time for the trigger. Bounded timing validity `0`/uncertainty `UINT32_MAX` mean no finite-accuracy certificate, not invalid imagery or physical synchronization. Explicit observer is a diagnostic/precision option retaining strict `500µs` capture and `200µs` projection admission with fatal failures. IMU tmst's `200µs` contract and IMU output bridges remain unchanged. Actual invalid metadata, clock-read/arithmetic, transport, ownership, and device failures still fail; availability-first does not guarantee arbitrary-load crash-free or loss-free operation. See [Verified Scope](https://4p-doc-en.readthedocs.io/en/4pv2.main/release-and-support.html#public-runtime-acceptance) for current default-mode capture, recording, stress, and reboot acceptance; the other three pairs and physical accuracy are not certified. Default YAML retains Web on, RTSP off, and auto exposure on. See [non-ROS Usage](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/non-ros-demo.html) for explicit manual exposure and the full contract.

The runtime also supports `./start_sensor_demo.sh stop` for a graceful stop, `./start_sensor_demo.sh restart` for background startup using package YAML after a successful stop, and `./start_sensor_demo.sh status` for package process/autostart status. Stop waits for recording and Web-child cleanup, with a default 1800-second timeout configurable through `SENSOR_DEMO_STOP_TIMEOUT_S`; timeout fails without force-killing or launching a second instance. Background output defaults to `/userdata/sensor_demo_autostart.log`. Restart success requires the selected cameras and IMU to keep advancing for at least 5 seconds. The startup health deadline defaults to 30 seconds and can be changed with `SENSOR_DEMO_START_TIMEOUT_S`. This check works independently of Web and logging and does not certify long-term stability. Management requires Linux pidfds and Python ≥3.10, does not change boot-autostart registration, and does not operate `cam-service`. See [Stop, Restart, and Boot Autostart](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/non-ros-demo.html#non-ros-autostart).

Boot autostart and `restart` override the package configuration with `--print-rate-hz 0`, excluding high-rate IMU sample blocks from background logs while retaining startup information, errors, recording results, and final health summaries. Native sampling, browser telemetry, and saved data are unchanged. Foreground `./sensor_demo` and argument-free `./start_sensor_demo.sh` still use YAML print settings; for diagnostics, use `./sensor_demo --print-rate-hz 10 --print-metrics`. Logs remain append-only: existing files are not cleared and no size limit is imposed.

The matching new system entry `/etc/init.d/S98sensor_demo_autostart` reads `/etc/4p/sensor-demo-autostart` (enabled when absent). `enable`/`disable` write system policy instead of editing shared `/userdata/startup.sh`. Normal reboot retains choices; reflashing system restores image defaults without importing old registration state. Update the matching system entry and runtime together, not just the launcher. Changes require a writable system partition; scripts never remount it automatically. Logs and recordings are not migrated or deleted.

### LAN Video and Six-Axis Preview

In current source, `sensor_demo` enables Web and disables RTSP by default. Open `http://<device-ip>/` from a computer on the same trusted LAN for four native H.264 videos, six-axis IMU values, and trends. Assets are embedded offline; no plugin is needed. The application owns and reaps `sensor_web`; do not launch it separately. This feature is included in the initial runtime package; default-mode acceptance does not cover all browsers, two-computer steady state, or GPUs.

The top-right language button shows the target language: `EN` on the Chinese page and `CH` on the English page. Chinese is the default; the browser remembers the choice when local storage is available. Switching preserves connections, recording options, and accepted bias. The documentation link opens the matching `4pv2.main` documentation in a new tab: [Chinese](https://4p-docs.readthedocs.io/zh-cn/4pv2.main/) / [English](https://4p-doc-en.readthedocs.io/en/4pv2.main/).

All functionality is on **one homepage**, without left-side page navigation or separate hash views. The workspace contains four cards: cameras, attitude, raw six-axis telemetry, and always-present “录制与保存” (Recording and Saving). At widths ≥1440px, all four are columns with aligned top and bottom edges and equal outer heights; cameras are largest and telemetry is slightly wider than attitude. At 1100–1439px, cameras/attitude/telemetry form three columns with recording in a full-width row below. At 768–1099px, cameras and recording each span the full width, while attitude and telemetry sit side by side. Below 768px, all cards stack. Calibration and relative-heading reset remain in the attitude card.

At a browser viewport ≥1440px wide and ≥680px high (CSS pixels), desktop uses a compact title and four health summaries, and all four cards fill the main workspace at equal height. The model and traces grow into the available space; overflow scrolls inside each panel. The video grid stays centered in the camera column. Narrow or short windows below either threshold and mobile devices use normal document scrolling.

Six-axis trends show a fixed one-second window without a window selector. Acceleration and angular velocity use separate plots, each sharing a time axis and automatically scaled vertical range across X/Y/Z. Labeled values retain raw m/s² or rad/s; the horizontal axis is seconds relative to browser now. No averaging or gravity/bias subtraction is applied; receive gaps over 250ms break traces. This changes preview only, not acquisition, saving, or attitude. Desktop overflow scrolls within the panel.

Expand “运行诊断” (Runtime Diagnostics) inline below the main workspace to inspect source states and actual per-camera decoder, frame-rate (fps), and packet-loss diagnostic text; missing fields still show `unknown`.

The camera card uses a small single-line “相机” (Cameras) heading aligned with resolution/frame rate/codec, without the `CAMERAS / 01` eyebrow. Per-camera header rows and expansion buttons are removed; CAM labels overlay the image's top-left corner and playback state its bottom-right corner. Double-click an image to expand/restore it, or focus it and press Enter or Space; `Esc` only exits single-camera expansion, not recording. Both grid and expansion use source-aspect `contain`, without cropping or stretching; source-image bars remain. Compact desktops maximize image size against the grid's remaining width and height.

The interface labels cameras **CAM1–CAM4**, corresponding to internal `camera_id` 0–3; APIs, recording filenames, and data indices are unchanged. Compact desktops enlarge the four frames at the source aspect ratio and reduce the camera card's horizontal padding; all four outer cards remain equal in height. The camera card has no explanatory footer. Side-by-side browser playback is still not a camera-display synchronization or FSYNC-accuracy acceptance test.

Expanding or collapsing recording details, advanced options, or diagnostics does not navigate, reload the document, recreate WebRTC connections, SSE subscriptions, or the attitude Worker, or reset current options or accepted gyro bias in the browser session. **Refreshing the browser still resets local options to page defaults and clears calibration**; these states are not persisted. Calibration and relative-heading reset are available in the attitude card and affect only the browser attitude preview; the layout adds no HTTP API, authentication, or hardware controls.

```bash
./sensor_demo --no-web --rtsp   # traditional RTSP only
./sensor_demo --web --rtsp      # both outputs share one encoder per camera
./sensor_demo --no-web --no-rtsp # no network output; saving remains independent
```

YAML `web.enabled` and `rtsp.enabled` are independent; `rtsp.bps/codec` remain shared encoding parameters. Web video supports H.264 only; H.265 is not transcoded and IMU telemetry remains available. HTTP uses TCP 80 and WebRTC uses UDP/TCP 8189; allowing HTTP alone is insufficient. Port conflicts degrade Web without taking over other services or stopping acquisition/saving.

After connecting a phone to the board AP, open `http://<AP-address>/` using the board IPv4 shown by `ip -4 addr show wlan0`, not the phone or wired address. With the default `0.0.0.0` bind, locally assigned addresses configured after startup are supported: HTTP Host must match the connection's actual local IP, and new WebRTC sessions use current interface addresses. An explicit IP bind serves only that address. Hostnames, query parameters, and foreign Origins are rejected. Use only trusted isolated networks.

There is no authentication: use only a trusted isolated LAN. HTTP signaling/telemetry lacks transport authenticity; do not expose it directly to the Internet or untrusted networks. Hardware decoding depends on the browser, OS, and driver; missing diagnostics show `unknown`, and a GPU does not imply four hardware-decoded streams. Side-by-side playback does not establish display or camera/IMU synchronization.

The always-present “录制与保存” (Recording and Saving) card contains one form. Its default view shows format, fixed storage, start/stop/query, device state, device-timed duration, and bytes written. A native “高级选项” (Advanced Options) details section is closed by default and contains frame skipping and minimum drop interval. A second, initially closed “录制详情与使用说明” (Recording Details and Usage) section contains free space, drop counts, final outcome, session ID, actual format/storage, output path, integrity/cleanup, health scope, and use boundaries. Actual errors, uncertain-command messages, and stopping/drain/remux messages stay outside the folds. Invalid hidden advanced values automatically open Advanced Options before browser validation feedback is shown. Existing validation, fresh-state/capability checks, command locks, and device acknowledgments/queries remain. Starting is unavailable when status is missing or stale, a command is pending or its outcome uncertain, or the device does not allow a start.

Select format and fixed storage in the recording card and start; expand Advanced Options to adjust frame skipping or minimum drop interval when needed. Use “停止录制” (Stop Recording) and wait for device drain/remux and its final result; only `published_complete`, `data_complete=true`, and `cleanup_complete=true` establish complete publication. Cameras, native IMU, and preview continue; sessions can be repeated. Closing/disconnecting the browser does not stop recording; reopen the page to query actual state, and never blindly retry after command timeouts. Runtime / eMMC uses the fixed absolute directory `/root/demo/save_data`, not a path derived from the ELF location: bag files use `/root/demo/save_data/session-<id>.bag`, and MP4 sessions use `/root/demo/save_data/session-<id>`, with device-allocated session names. TF remains `/mnt/sdcard/recordings` on a mounted card; a missing/unmounted card explicitly fails without falling back to eMMC. Web accepts no custom paths, and symlink security constraints are unchanged. Old data stays in its original directory, without automatic moves or deletion. Bag/MP4 are mutually exclusive; MP4 requires four-camera H.264. API and native-data contracts are unchanged. CLI/YAML startup saving, `save_data.save_path` semantics, and the default `/root/demo/save_mp4/` remain; startup saving can be stopped from Web. Saving does not use browser-decimated/attitude-filtered data, and dynamic MP4 health covers only `callback_window`. With no authentication, anyone reaching the device can start/stop disk writes. See [Saving Data](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/save-data-guide.html).

The page provides client-side attitude preview: a browser Web Worker fuses dedicated ≤50Hz six-axis inputs into a quaternion, showing Roll/Pitch/relative Yaw. The device computes neither attitude nor bias; it only applies four cascaded one-pole low-pass stages to every native sample before decimation. Each pole is `min(10Hz, telemetry limit×0.2, source rate×0.2)`, with about 64ms low-frequency group delay at the default 800→50Hz. SSE adds `attitude_accel_mps2` / `attitude_gyro_rps`; raw six-axis value/trace fields, native sampling, saving, and public ABI are unchanged. World +Z is up, and initialization follows current gravity. Both raw and filtered acceleration must pass the gravity band for tilt correction; otherwise only gyros integrate.

On first opening the page or entering a new source session, keep the **actual device completely stationary**. The Worker automatically initializes three-axis gyro bias after about **3 seconds** of continuous low-motion, gravity-consistent telemetry, without requiring a button click. It checks a minimum sample count, continuous timestamps, gravity/angular-rate magnitude, accelerometer/gyro variance, and first-versus-last segment changes in both accelerometer and gyro means. Detected motion, strong vibration, sparse data, or timestamp gaps reset the candidate window; no learning occurs during detected motion.

Integration and bias means use the inputs filtered before decimation; stationary-quality checks still use raw six-axis values, so filtering cannot conceal strong vibration. Non-increasing time, a sequence reset, or a native-sample gap over 100ms reseeds the filter. Invalid input makes attitude fields unavailable; missing samples are not extrapolated.

Once bias is known, conservative automatic updates require a complete approximately 3-second stationary window whose **mean residual angular-rate vector magnitude is ≤0.002 rad/s (about 0.11°/s)**. Exponential tracking with a **6-second time constant** follows gradual bias changes; accepting an automatic estimate does not re-zero heading or jump attitude. Status text indicates waiting/collection progress for automatic stationary estimation and estimated/tracking states. These states describe the checks, not proof of actual stillness or absolute-heading accuracy.

With known bias, automatic tracking also checks raw mean residual angular rates over approximately 1-second sections and the final partial section, so the pre-decimation filter's latency cannot hide a late turn. Excessive noise defers updates instead of freezing heading. Invalid six-axis, attitude-input, or time fields discard partial automatic windows and interrupt manual calibration while retaining accepted bias within the same session.

The “静置校准零偏” (Calibrate gyro bias while stationary) button remains available for manual re-estimation: stop the device completely, click, and keep it stationary for about **3 seconds** until the page reports the result. Motion, instability, vibration, or sparse data can reject the attempt without replacing previously accepted bias. Keep still, wait for stable telemetry, and retry. After temperature changes, stationary dwell allows automatic tracking, or you can explicitly recalibrate.

Every quaternion integration step subtracts accepted three-axis bias first. **There is no stationary Yaw freeze, hard angular-rate deadband, or display-only damping.** “重置相对航向” (Reset relative heading) realigns current gravity rather than zeroing tilt, retaining accepted bias within the same application session. After 500ms without valid attitude the model freezes; reconnection, non-increasing timestamps, or gaps over 250ms realign it. Time discontinuities cancel incomplete candidate windows/manual calibration but preserve accepted bias within the same source session. A new source session or page reload clears bias, which is automatically re-estimated after another approximately 3-second stationary dwell.

Six-axis data cannot fully distinguish bias from steady slow rotation: automatic tracking may absorb slow residual rotation below the threshold above. Initial estimation and manual calibration likewise require actual stillness; neither perfect motion detection nor preservation of arbitrarily slow motion is claimed. After calibration, genuine rotation ≥0.005 rad/s (about 0.29°/s) must remain measurable rather than being removed as bias. Without an external heading reference, Yaw is relative only, unobservable, and still drifts; no absolute or drift-free heading is provided. This is not for navigation or a replacement for full sensor calibration. Motion acceleration, vibration, residual bias, and telemetry extraction affect preview accuracy.

To deploy this update, rebuild `sensor_demo`, the `sensor_web` helper containing the embedded page/Worker, and the full runtime package, replace the package as a matched set, and restart the application normally. A browser refresh alone does not update the running embedded code. These instructions do not establish board deployment or acceptance.

Deployment here means only an authorized local runtime-package update on the user's device, not a new software-version release or an expansion of the existing acceptance scope.

`sensor_web` uses Rust and str0m in a static ARM64 musl executable with embedded browser assets. Four-camera video, UDP/passive TCP ICE, IMU SSE, and recording endpoints are retained. Source builds require exact Rust `1.95.0`, cargo-zigbuild `0.23.0`, Zig `0.16.0`, the `aarch64-unknown-linux-musl` target, and a pre-provisioned crate cache; select Cargo with `SENSOR_WEB_CARGO_EXECUTABLE`. Actual builds are frozen/offline and never download or switch toolchains automatically. The complete runtime includes `bin/sensor_web` and third-party licenses; the device needs neither Rust nor Go. CPU/RSS improvements require same-device, same-load measurements.

Final static linkage uses musl `1.2.5` / mallocng and `rust-lld` bundled with that Rust target; Zig compiles native C/assembly dependencies only, not the final libc. The verifier requires native mallocng symbols and rejects the old allocator or binaries stripped of those symbols. This permits idle allocation groups to return to the kernel; closing the page does not promise zero RSS. Rebuild the complete package and restart the application normally; a browser refresh alone is insufficient.

### USB Camera Source and Rebuilds

Four-camera UVC is separate from `sensor_demo` and root CMake builds, and **does not need the Web service's Rust toolchain**. In [`uvc_mosaic.cpp`](scripts/env_setup/x5_usb/uvc_mosaic.cpp), start with `Source::Start()` (acquisition), `Compose()` (mosaic), and `Encoder` (hardware JPEG). USB descriptors and backend start/stop are in [`switch_usb_gadget`](scripts/env_setup/x5_usb/switch_usb_gadget), including `uvc()`. Cross-compile with [`build_uvc.sh`](scripts/env_setup/x5_usb/build_uvc.sh), the matching X5 SDK, and this repository's ABI3 headers/libraries. [`install.py`](scripts/env_setup/x5_usb/install.py) accepts only an offline rootfs and a staged `--package` directory; runtime scripts are [`S98rootfs.sh`](scripts/env_setup/x5_usb/S98rootfs.sh) and [`S99usb.sh`](scripts/env_setup/x5_usb/S99usb.sh). It is not for the live `/`.

See the [USB Usage and Development Guide](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/usb-gadget-switch.html#usb-uvc-development) for complete commands, camera-resource handoff, and safe updates on an existing board. Rebuilding other mosaic examples does not update `/usr/bin/x5-uvc-mosaic`. Application-logic changes with an unchanged output profile and existing kernel/device-tree support normally need no reflash. The tools do not deploy automatically, and source changes do not establish publication or on-device video-reception acceptance.

## Documentation

- [Source Builds and Runtime Verification](https://4p-doc-en.readthedocs.io/en/4pv2.main/development/open-source-build.html)
- [Data Persistence](https://4p-doc-en.readthedocs.io/en/4pv2.main/usage/save-data-guide.html)
- [Data Contracts](https://4p-doc-en.readthedocs.io/en/4pv2.main/development/data-contracts.html) · [API Reference](https://4p-doc-en.readthedocs.io/en/4pv2.main/development/api-reference.html)
- [Hardware and Safety](https://4p-doc-en.readthedocs.io/en/4pv2.main/getting-started/hardware-and-safety.html)
- [Troubleshooting](https://4p-doc-en.readthedocs.io/en/4pv2.main/troubleshooting.html)
- [Versions, Known Limitations, and Support](https://4p-doc-en.readthedocs.io/en/4pv2.main/release-and-support.html)

See this repository's [LICENSE](LICENSE) and public release notes for licensing and third-party components.

## Independent FSYNC diagnostics

ABI6 requires the matching kernel IIO V4 module. `fsync_diagnostics` accepts `0/1`, default `1`. Non-ROS uses `imu.fsync_diagnostics: true|false` or `--imu-fsync-diagnostics on|off`; ROS2 uses the read-only startup bool `imu.fsync_diagnostics`. Stop normally and restart to change it. The standalone reader uses `--fsync-diagnostics on|off`; `imu_reader_demo` uses `--imu-fsync-diagnostics on|off`.

Six fields extend the previous sample layout: `fifo_header`, `fsync_flags`, `fsync_sequence`, `fsync_raw_ns`, `fsync_uncertainty_ns`, and `fsync_delta_us`. FIFO headers masked by `0xfc`, with values `0x68` and `0x6c` identify ordinary packets and real FSYNC tags. For tagged packets, `fifo_tmst` holds the FSYNC delta in microseconds, not an absolute sample tick. Flag bits are `1=ENABLED`, `2=TAGGED`, `4=TIME_VALID`, and `8=UNRESOLVED`. Ordinary samples have event sequence zero; real-tag event sequences increase within the session. An invalid edge must retain `fsync_raw_ns=0` and `fsync_uncertainty_ns=UINT32_MAX`, never interpreted as zero error.

`fsync_raw_ns` is an independent RAW-clock edge diagnostic. It neither corrects host sample timestamps nor automatically associates camera frames. Replaced absolute ticks are reconstructed from measured left/right endpoints, using nominal ODR only for admissible intervals. Host-mode diagnostic mapping failure retains unresolved tags and allows ordinary acquisition to continue; tmst still requires valid mapping and fails otherwise. Bag/ROS2 `robobaton_imu_timing_v3` and MP4 `imu.csv` retain all six fields, without a separate FSYNC file. This contract certifies neither elimination of frame latency tails, LPWM waveforms, nor physical synchronization within 1 ms.
