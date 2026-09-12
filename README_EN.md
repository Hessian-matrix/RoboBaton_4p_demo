# X5 SC132 4-Camera, IMU, and UART Demo

中文说明：[README.md](README.md)

![RoboBaton 4P](image/4P_Cam.png)

This is the public non-ROS demo repository for RoboBaton 4P. It provides the X5 runtime package, SC132 four-camera RTSP examples, ICM-42688 IMU examples, UART1/UART7 examples, public headers, and matching prebuilt runtime libraries.

> **The [4P_doc](https://4p-docs.readthedocs.io/en/latest/index.html) documentation is authoritative for end users.** This README keeps only the repository entry point, minimum run path, and support boundaries. Use the documentation site for deployment, persistence, wiring, data/API contracts, and troubleshooting.

## Contents

```text
demo/       Complete X5 package for `/root/demo`
include/    Public C headers
lib/        Prebuilt libraries matching the current demo and headers
config/     Default `sensor_demo` YAML
src/        Demo source
scripts/    Build, packaging, and runtime verification entry points
```

Users normally need only `demo/`. Do not replace only one ELF, one `.so`, or the configuration file.

## Version and package matching

Use the repository, `demo/`, prebuilt libraries, and public documentation as one release composition. Confirm `VERSION`, the repository tag, and `demo/manifest.sha256`; do not mix source, headers, shared libraries, and runtime packages from different releases.

On the board, query program and runtime-library versions:

```bash
cd /root/demo
./cam_demo --version
./sensor_demo --version
./imu_reader_demo --version
./serial_port_demo --version
```

These version queries do not initialize the camera, IMU, or UART. For compatibility, ABI, and release details, see [Product and compatibility](https://4p-docs.readthedocs.io/en/latest/product-and-compatibility.html), [API reference](https://4p-docs.readthedocs.io/en/latest/api-reference.html), and [Changelog](https://4p-docs.readthedocs.io/en/latest/changelog.html).

## Choose a demo

| Program | Purpose |
|---|---|
| `sensor_demo` | Four cameras, RTSP, and IMU together; optional ROS1 bag or H.264 MP4 persistence |
| `cam_demo` | Four cameras and RTSP |
| `imu_reader_demo` | Standalone IMU reading |
| `serial_port_demo` | UART1/UART7 example; not for DEBUG_UART |

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

IMU only:

```bash
./imu_reader_demo
```

UART1/UART7 example:

```bash
./serial_port_demo
```

For first power-on, network setup, deployment prerequisites, and the required `cam-service`, see [First power-on](https://4p-docs.readthedocs.io/en/latest/first-boot.html) and [Quick start](https://4p-docs.readthedocs.io/en/latest/quick-start.html).

## Persistence

`sensor_demo` does not persist data automatically. The default format field is MP4 and the default path is `/root/demo/save_mp4/`. Explicitly select one mode when enabling persistence:

```bash
# ROS1 bag v2.0
./sensor_demo --record-bag /data/run.bag

# H.264 MP4 session
./sensor_demo --record-mp4-dir /data/mp4_session
```

ROS1 bag and MP4 are mutually exclusive. MP4 supports H.264 and the complete four-camera mask; frame skip applies only to ROS1 bag. For output files, stop order, completeness rules, partial/recovery handling, and offline extraction, see [Data persistence](https://4p-docs.readthedocs.io/en/latest/save-data-guide.html).

## Build from source

This repository is cross-compiled on the development host and the generated package runs on X5. Before building, provide matching `./include`, `./lib`, the X5 cross-compilation package, and host CMake:

```bash
export TOOLCHAIN_FILE="/path/to/aarch64_x5_host_toolchain.cmake"
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
cmake --build build_x5 -j
```

Maintainers refresh the complete runtime package with:

```bash
scripts/package_runtime.sh --toolchain-file "$TOOLCHAIN_FILE"
python3 scripts/verify_runtime_package.py demo
```

For toolchain preparation, dependencies, build boundaries, and package verification, see [Open-source demo build](https://4p-docs.readthedocs.io/en/latest/open-source-build.html).

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

Do not delete `/root/demo` before uploading a new package, and do not test the camera with `cam-service` stopped. For the complete procedure, see [Deployment, upgrade, and rollback](https://4p-docs.readthedocs.io/en/latest/deployment-and-upgrade.html).

## Support-boundary summary

- Camera output is NV12 `1280x1088`; RTSP defaults to H.264, with optional H.265, path `/PRR`, and default ports `554..557`.
- The public camera set is `25/30/40/50/60fps`, default `30fps`; `rotate=180` is supported only at `30fps`.
- The IMU supports `25/50/100/200/500/1000/2000Hz`, default `1000Hz`, and uses the sensor-timestamp FIFO path. TF and calibration are not provided.
- DEBUG_UART is `1.8V`; UART1/UART7 are `3.3V`. Their `3V3` pins support input/output and peripheral power; the two interfaces share a formal `500mA` limit and support hot-plugging. Hardware communication passed V1 acceptance.
- Camera operation requires the X5 board-side `cam-service`; this is not a general-purpose host package.

For complete camera, IMU, UART, timestamp, frame-rate, and data semantics, see [Data contracts](https://4p-docs.readthedocs.io/en/latest/data-contracts.html), [non-ROS Demo usage](https://4p-docs.readthedocs.io/en/latest/non-ros-demo.html), and [Hardware and safety](https://4p-docs.readthedocs.io/en/latest/hardware-and-safety.html).

## Troubleshooting

When startup, shared-library, RTSP, IMU, or UART problems occur, retain `VERSION`, manifest results, the command, exit code, and necessary logs. Do not submit real IPs, credentials, or internal paths. Start with [Troubleshooting](https://4p-docs.readthedocs.io/en/latest/troubleshooting.html).

## Public documentation index

- [Product introduction](https://4p-docs.readthedocs.io/en/latest/Product_Introduction.html)
- [Product and compatibility](https://4p-docs.readthedocs.io/en/latest/product-and-compatibility.html)
- [First power-on](https://4p-docs.readthedocs.io/en/latest/first-boot.html)
- [Quick start](https://4p-docs.readthedocs.io/en/latest/quick-start.html)
- [non-ROS Demo usage](https://4p-docs.readthedocs.io/en/latest/non-ros-demo.html)
- [Deployment, upgrade, and rollback](https://4p-docs.readthedocs.io/en/latest/deployment-and-upgrade.html)
- [Open-source demo build](https://4p-docs.readthedocs.io/en/latest/open-source-build.html)
- [Data persistence](https://4p-docs.readthedocs.io/en/latest/save-data-guide.html)
- [Data contracts](https://4p-docs.readthedocs.io/en/latest/data-contracts.html)
- [API reference](https://4p-docs.readthedocs.io/en/latest/api-reference.html)
- [Hardware and safety](https://4p-docs.readthedocs.io/en/latest/hardware-and-safety.html)
- [Troubleshooting](https://4p-docs.readthedocs.io/en/latest/troubleshooting.html)
- [Changelog](https://4p-docs.readthedocs.io/en/latest/changelog.html)

License and third-party component information are defined by this repository's `LICENSE` and release documentation.
