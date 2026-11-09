# RoboBaton 4P non-ROS Demo

[中文](README.md) · [English documentation](https://4p-doc-en.readthedocs.io/en/latest/index.html)

X5 examples for SC132 four-camera capture, RTSP, ICM-42670-P IMU, UART, and SocketCAN. Wiring, deployment, parameters, recording, APIs, and troubleshooting are maintained in the public documentation repository [4P_doc](https://github.com/Hessian-matrix/4P_doc).

## Repository Contents

- `demo/`: complete X5 runtime package with `sensor_demo`, `cam_demo`, `mosaic_rtsp_demo`, `imu_reader_demo`, `serial_port_demo`, and `can_demo`.
- `src/`, `include/`, `lib/`, `config/`: example sources, public headers, matching prebuilt libraries, and default configuration.
- `scripts/`: build, packaging, and runtime verification entry points.

Current IMU capture uses kernel IIO V4 / public ABI 6.0: real file `libicm42670.so.6.0.0`, SONAME `libicm42670.so.6`, and nine exports under `ICM42670_X5_6.0`. `host` retains the `CLOCK_MONOTONIC` host estimate; `tmst` additionally maps sensor TMST to `CLOCK_MONOTONIC_RAW`. Both preserve native host time and all burst evidence. Saved output explicitly bridges the selected native clock to realtime per burst, retaining the bridge bracket half-width without certifying physical sampling or exposure synchronization accuracy. The matching V4 kernel module must already bind the device; V2/V3 is incompatible even in host mode. No userspace SPI, trigger-to-sample association, or automatic fallback is provided. Rebuild every consumer with matching headers/libraries, without cross-major links. Software, documentation, ABI, and release status remain independent. Independent FSYNC diagnostics default on: YAML `imu.fsync_diagnostics: true`, or `sensor_demo`/`imu_reader_demo --imu-fsync-diagnostics on|off`; ROS2 startup-only parameter `imu.fsync_diagnostics=true`. Timing schema v3 and MP4 IMU CSV preserve `fifo_header,fsync_flags,fsync_sequence,fsync_raw_ns,fsync_uncertainty_ns,fsync_delta_us`; RAW-edge diagnostics never correct sample/camera time or match frames.

This repository builds user-side examples, not the underlying runtime libraries. Keep sources, headers, libraries, and configuration matched. Deploy the complete runtime package, preserving symbolic links; do not replace individual `.so` files.

## Getting Started

Prepare the device and runtime package using [First Power-On](https://4p-doc-en.readthedocs.io/en/latest/getting-started/first-boot.html) and [Deployment, Upgrade, and Rollback](https://4p-doc-en.readthedocs.io/en/latest/development/deployment-and-upgrade.html), then run on X5:

```bash
cd /root/demo
./sensor_demo --version

./sensor_demo
```

Press `Ctrl+C` to stop normally. Keep `cam-service` running and do not run multiple camera applications concurrently. See [non-ROS Demo Usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html) for other examples and complete parameters.

The final default is HO: camera `observer` + IMU `host`, with IMU at `800Hz`. The other three startup combinations remain for comparison: top-level YAML `camera_timestamp_path: observer|official` and `imu_timestamp_mode: host|tmst`, overridden by CLI `--camera-timestamp-path` / `--imu-timestamp-mode`. Stop normally and restart after changing them; there is no hot switching or failure fallback. Camera observer's `500µs` and IMU tmst's `200µs` are separate computational admission limits; official has unknown physical error. Default YAML retains Web on, RTSP off, and auto exposure on. See [non-ROS Usage](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html) for explicit manual exposure and the full contract.

The runtime also supports `./start_sensor_demo.sh stop` for a graceful stop, `./start_sensor_demo.sh restart` for background startup using package YAML after a successful stop, and `./start_sensor_demo.sh status` for package process/autostart status. Stop waits for recording and Web-child cleanup, with a default 1800-second timeout configurable through `SENSOR_DEMO_STOP_TIMEOUT_S`; timeout fails without force-killing or launching a second instance. Background output defaults to `/userdata/sensor_demo_autostart.log`. Restart success means only that the process did not exit within 2 seconds; inspect logs and the page. Management requires Linux pidfds and Python ≥3.10, does not change boot-autostart registration, and does not operate `cam-service`. See [Stop, Restart, and Boot Autostart](https://4p-doc-en.readthedocs.io/en/latest/usage/non-ros-demo.html#non-ros-autostart).

### LAN Video and Six-Axis Preview

In current source, `sensor_demo` enables Web and disables RTSP by default. Open `http://<device-ip>/` from a computer on the same trusted LAN for four native H.264 videos, six-axis IMU values, and trends. Assets are embedded offline; no plugin is needed. The application owns and reaps `sensor_web`; do not launch it separately. This feature has not been released as a new software version or accepted across all browsers, two-computer steady state, or GPUs.

The top-right language button shows the target language: `EN` on the Chinese page and `CH` on the English page. Chinese is the default; the browser remembers the choice when local storage is available. Switching preserves connections, recording options, and accepted bias. The documentation link opens the matching language in a new tab.

All functionality is on **one homepage**, without left-side page navigation or separate hash views. The workspace contains four cards: cameras, attitude, raw six-axis telemetry, and always-present “录制与保存” (Recording and Saving). At widths ≥1440px, all four are columns with aligned top and bottom edges and equal outer heights; cameras are largest and attitude is slightly wider than telemetry. At 1100–1439px, cameras/attitude/telemetry form three columns with recording in a full-width row below. At 768–1099px, cameras and recording each span the full width, while attitude and telemetry sit side by side. Below 768px, all cards stack. Calibration and relative-heading reset remain in the attitude card.

At a browser viewport ≥1440px wide and ≥680px high (CSS pixels), desktop uses a compact title and four health summaries, and all four cards fill the main workspace at equal height. The model and traces grow into the available space; overflow scrolls inside each panel. The video grid stays centered in the camera column. Narrow or short windows below either threshold and mobile devices use normal document scrolling.

Six-axis trends default to the last 2 seconds, with 5- and 10-second choices. X/Y/Z have separate traces, independently autoscaled over the visible window with three labeled vertical ticks. Vertical axes retain absolute raw values in the title's m/s² or rad/s; horizontal axes show seconds relative to browser now. Ranges may differ, so trace heights cannot directly compare amplitudes across axes. No averaging or gravity/bias subtraction is applied; receive gaps over 250ms break the trace. Only preview changes, not sampling, saving, or attitude. Desktop overflow scrolls within the panel; refresh restores 2 seconds.

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

Web may start before USB/Wi-Fi networking is ready. Once reachable, open the device IPv4 address actually reached by that connection; newly available addresses do not require an application restart. HTTP Host must match the connection's local address, and an explicit port must match configuration. Hostnames, reverse-proxy Host values, and another interface's address as Host are unsupported. Same-origin checks remain enforced. New WebRTC sessions gather candidates from currently available interfaces; the page reconnects after an existing session loses connectivity.

There is no authentication: use only a trusted isolated LAN. HTTP signaling/telemetry lacks transport authenticity; do not expose it directly to the Internet or untrusted networks. Hardware decoding depends on the browser, OS, and driver; missing diagnostics show `unknown`, and a GPU does not imply four hardware-decoded streams. Side-by-side playback does not establish display or camera/IMU synchronization.

The always-present “录制与保存” (Recording and Saving) card contains one form. Its default view shows format, fixed storage, start/stop/query, device state, device-timed duration, and bytes written. A native “高级选项” (Advanced Options) details section is closed by default and contains frame skipping and minimum drop interval. A second, initially closed “录制详情与使用说明” (Recording Details and Usage) section contains free space, drop counts, final outcome, session ID, actual format/storage, output path, integrity/cleanup, health scope, and use boundaries. Actual errors, uncertain-command messages, and stopping/drain/remux messages stay outside the folds. Invalid hidden advanced values automatically open Advanced Options before browser validation feedback is shown. Existing validation, fresh-state/capability checks, command locks, and device acknowledgments/queries remain. Starting is unavailable when status is missing or stale, a command is pending or its outcome uncertain, or the device does not allow a start.

Select format and fixed storage in the recording card and start; expand Advanced Options to adjust frame skipping or minimum drop interval when needed. Use “停止录制” (Stop Recording) and wait for device drain/remux and its final result; only `published_complete`, `data_complete=true`, and `cleanup_complete=true` establish complete publication. Cameras, native IMU, and preview continue; sessions can be repeated. Closing/disconnecting the browser does not stop recording; reopen the page to query actual state, and never blindly retry after command timeouts. Runtime storage is `recordings` under the real ELF directory (standard package `/root/demo/bin/recordings`); TF is mounted `/mnt/sdcard/recordings`, without arbitrary paths. Bag/MP4 are mutually exclusive; MP4 requires four-camera H.264. Storage, API, and native-data contracts are unchanged. Startup CLI/YAML saving remains and can be stopped from Web; saving does not use browser-decimated/attitude-filtered data, and dynamic MP4 health covers only `callback_window`. With no authentication, anyone reaching the device can start/stop disk writes. See [Saving Data](https://4p-doc-en.readthedocs.io/en/latest/usage/save-data-guide.html).

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

Source builds additionally require exact Go `1.27.3`, a pre-provisioned module cache, and `SENSOR_WEB_GO_EXECUTABLE`; builds do not download dependencies or switch toolchains automatically. The complete runtime includes ARM64 `bin/sensor_web` and third-party licenses; the device needs no Go installation.

### Calibrated 360° Panorama

The independent `panorama_demo` is a geometric panorama, not the four-tile `mosaic_rtsp_demo` or a new `sensor_demo` homepage mode. Gracefully stop other camera applications, keep `cam-service` running, and run from a freshly built complete package:

```bash
./panorama_demo --calibration config/calibration.yaml --port 8080
```

Open `http://<device-ip>:8080/` on a trusted isolated LAN for one equirectangular image, geometric coverage, processing time, and dropped groups. Stop with `Ctrl+C`. HTTP has no authentication or encryption; never expose it to public networks. The program neither acquires IMU nor records data nor starts/stops other applications.

Calibration requires schema 7, `kannala-brandt4` intrinsics, and `T_camN_from_imu` poses (wxyz, metres). cam0..3 must match the calibrated fixed assembly and pixel orientation. The example is `pending_human_review`, not calibration for arbitrary devices. Defaults: 1600×600, 360° azimuth, ±60° elevation. Centre is IMU +X, positive azimuth toward +Y and positive elevation toward +Z; no gravity levelling. `--yaw` changes centre azimuth. `--distance-m 0` uses rotation-only infinity; positive values ≥0.1m use an IMU-centred sphere and extrinsic translation. Each frame robustly estimates brightness ratios from calibrated overlaps and solves common exposure gains, bounded to 0.5–2, for reliably connected views. Dark, clipped, and strongly chromatically inconsistent samples are excluded; cameras without reliable evidence retain their brightness. Main pixels select one source by optical-axis/source-border quality. Near ownership boundaries, a continuous seam search uses local color disagreement, structural edges, and projection quality to avoid inconsistent foreground edges, with a soft penalty for movement from the previous processed frame. Broad smooth compensation removes low-frequency illumination differences around the seam and fades at valid overlap/ownership edges. High-frequency detail remains single-view; only colors agreeing after compensation blend within four output pixels on either side, avoiding wide foreground ghosting. The 360° join uses the same wrapped processing. RGB/NV12 share photometric correction and source selection without sharpening, image blur, optical-flow deformation, or changes to calibrated sampling coordinates. Compensation does not control camera exposure or restore clipped/crushed details. Missing reliable common content, strong chromatic disagreement, and parallax can leave seams; motion is not guaranteed flicker-free. Near objects can still misalign or break. Blind regions remain black. `100%` geometric horizon coverage does not certify full-sphere coverage, valid scene pixels, or calibration accuracy.

Capture stays at four-channel 30fps; processing uses only the latest complete group, capped by `--max-fps 10` by default. Slow processing intentionally drops groups; no board throughput is promised. Rendering reuses at most three helper threads plus the caller for disjoint pixel regions, traverses seam-filter rows contiguously, and precomputes calibrated quality terms without reducing output resolution. `/frame.jpg` and `/status.json` are latest-only read-only snapshots, not a physical synchronization acceptance surface.

Host offline builds require C++17, CMake, LibYAML/JPEG development headers and static archives, without X5 or Go:

```bash
cmake -S . -B build_panorama_host -DPANORAMA_OFFLINE_ONLY=ON
cmake --build build_panorama_host -j
./build_panorama_host/panorama_demo --calibration config/calibration.yaml \
  --images /path/to/group --once --output /path/to/new-panorama.jpg
```

The input directory must contain `cam0.jpg`..`cam3.jpg` from one group in calibrated dimensions/orientation; filenames alone do not establish synchronization. Omit `--once` to serve the static image through the same Web page. `--output` refuses existing paths. Full X5 builds also need target LibYAML/JPEG headers/static archives; the standard package includes the new entry, calibration, and notices, with neither library installation required on-device. This uses calibrated projection, photometric correction, and seam blending, not depth reconstruction or automatic recalibration. JPEG handling is based in part on the work of the Independent JPEG Group; see `licenses/panorama/THIRD_PARTY_NOTICES.txt`.


## Documentation

- [Source Builds and Runtime Verification](https://4p-doc-en.readthedocs.io/en/latest/development/open-source-build.html)
- [Data Persistence](https://4p-doc-en.readthedocs.io/en/latest/usage/save-data-guide.html)
- [Data Contracts](https://4p-doc-en.readthedocs.io/en/latest/development/data-contracts.html) · [API Reference](https://4p-doc-en.readthedocs.io/en/latest/development/api-reference.html)
- [Hardware and Safety](https://4p-doc-en.readthedocs.io/en/latest/getting-started/hardware-and-safety.html)
- [Troubleshooting](https://4p-doc-en.readthedocs.io/en/latest/troubleshooting.html)
- [Versions, Known Limitations, and Support](https://4p-doc-en.readthedocs.io/en/latest/release-and-support.html)

See this repository's [LICENSE](LICENSE) and public release notes for licensing and third-party components.
