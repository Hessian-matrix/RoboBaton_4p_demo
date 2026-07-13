# RoboBaton 4P Demo Release Checklist

> Internal checklist only. Do not publish this file in the release repository.

## 1. Source Tree

- [ ] `git status --short --ignored` reviewed.
- [ ] `README.md` and `README_EN.md` describe the same build/deploy flow.
- [ ] No customer-facing source/header contains internal change-record comments such as `修改原因`.
- [ ] `include/pr_venc.h` exposes only RTSP APIs; MP4 APIs are not in the delivered header.
- [ ] `include/sc132camera.h` exposes only camera/frame callback APIs used by the demo.
- [ ] `include/icm42688_driver.h` exposes only the stable IMU driver API.

## 2. Build

```bash
cd /root/x5/4cam/sub_module/RoboBaton_4p_demo
cmake -S . -B build_x5 \
  -DCMAKE_TOOLCHAIN_FILE=/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake
cmake --build build_x5 -j
```

- [ ] `build_x5/cam_demo` exists.
- [ ] `build_x5/imu_reader_demo` exists.
- [ ] `build_x5/serial_port_demo` exists.
- [ ] Single-target scripts work:

```bash
scripts/build_cam_demo.sh
scripts/build_imu_reader_demo.sh
scripts/build_serial_port_demo.sh
```

## 3. Binary Hygiene

```bash
file build_x5/cam_demo build_x5/imu_reader_demo build_x5/serial_port_demo
file lib/libicm42688.so lib/libsc132.so lib/libprrtsp.so
nm -D --defined-only lib/libsc132.so | awk '{print $3}' | sort
nm -D --defined-only lib/libprrtsp.so | awk '{print $3}' | sort
nm -D --defined-only lib/libicm42688.so | awk '{print $3}' | sort
```

- [ ] All binaries are ARM aarch64.
- [ ] `libsc132.so` exports only public camera/frame APIs.
- [ ] `libprrtsp.so` exports only the default RTSP channel and `ch1..ch4`; no `ch5` symbols are exported.
- [ ] MP4 symbols are not exported from `libprrtsp.so`.
- [ ] `.so` files are stripped or contain no ordinary debug symbol table.
- [ ] `strings lib/libprrtsp.so | grep ch5` has no output.

## 4. Runtime Package

```bash
scripts/package_runtime.sh --build-dir build_x5 --output-dir demo
find demo -maxdepth 2 -type f | sort | xargs sha256sum
```

- [ ] Runtime package contains top-level launchers.
- [ ] Runtime package contains `env.sh`.
- [ ] Runtime package contains real ELF binaries under `bin/`.
- [ ] Runtime package contains only required `.so` files under `lib/`.

Expected layout:

```text
demo/
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

## 5. Board Smoke Test

```bash
ssh root@<x5-ip> "rm -rf /root/demo && mkdir -p /root/demo"
tar -C demo -cf - . | ssh root@<x5-ip> "tar -xf - -C /root/demo"
ssh root@<x5-ip> "cd /root/demo && . ./env.sh && ldd ./bin/cam_demo"
ssh root@<x5-ip> "cd /root/demo && ./cam_demo --help"
ssh root@<x5-ip> "cd /root/demo && ./imu_reader_demo --help"
ssh root@<x5-ip> "cd /root/demo && ./serial_port_demo --help"
```

- [ ] `LD_LIBRARY_PATH` starts with `/root/demo/lib`.
- [ ] `ldd ./bin/cam_demo` loads `/root/demo/lib/libsc132.so`.
- [ ] `ldd ./bin/cam_demo` loads `/root/demo/lib/libprrtsp.so`.
- [ ] No `not found` appears in `ldd ./bin/cam_demo`.
- [ ] All three `--help` commands exit successfully.

## 6. Camera Regression

```bash
X5_PASS=<password> scripts/cam_demo_regression.sh \
  --host <x5-ip> \
  --remote-dir /root/demo \
  --kill-existing
```

- [ ] RTSP ports `554/555/556/557` open.
- [ ] Four SC132 sensors are detected.
- [ ] Four encoders initialize successfully.
- [ ] No segmentation fault, undefined symbol, GLIBCXX, `ret=-36`, or `ret=-10`.
- [ ] Per-channel FPS meets threshold.
- [ ] `full_waits` remains 0.
- [ ] Frame-set `frame_id` offset jitter remains 0.
