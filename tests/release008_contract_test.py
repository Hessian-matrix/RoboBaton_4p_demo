#!/usr/bin/env python3
"""RELEASE-008 non-ROS production-bound host contract and lifecycle gate."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
ICM_ROOT = Path(os.environ.get("ICM42688_MODULE_ROOT", "/tmp/ABI-008-ICM-DIRECT-RELEASE008/artifacts/icm42688"))
SC_ROOT = Path(os.environ.get("SC132_MODULE_ROOT", "/tmp/VERIFY-009-SC132-003-module-L7qS1t/sc132"))
RTSP_ROOT = Path(os.environ.get("PRRTSP_MODULE_ROOT", "/tmp/ABI-007C-board-rc-direct-sVfZ5p/artifacts/prrtsp"))
RETIRED_SC_ROOT = Path("/tmp/ABI-008-SC132-v2-fix-direct-XVjOlC/artifacts/sc132")
REPLACEMENT_SC_MANIFEST_SHA256 = "8440de56ee9e759ca6d1f5c7d4436595bc20451d49e8ac61b943980264a372a5"
REPLACEMENT_SC_ELF_SHA256 = "1aaafaadb1a52e7fa40928c1f40fc815bf7d6ffa8b98be175fa1e470b70bc624"
REPLACEMENT_SC_BUILD_ID = "a7aad111380b7d408cd3ffd95a46a7958884fd98"
RETIRED_SC_MANIFEST_SHA256 = "7152bb9e4a6fc228e11d04c48c564f98eaefe2ffac021475f08e7f8d45cfee70"
RETIRED_SC_ELF_SHA256 = "9028e8c49753877565a13b1cfda20c91ac5ea8e534158fe7877947b903cb3274"
# VERIFY-009-SC132-003 contract additionally names a retired 9dc0e9b.../18fd52...
# generation. Exact active allowlisting must reject it even when those retired bytes are absent.
STIPULATED_RETIRED_SC_PREFIXES = ("9dc0e9b", "18fd52")
RETIRED_RTSP_ROOT = Path("/tmp/ABI-007C-board-vbv-repro-final.OwnYIB/artifacts-1/prrtsp")
RTSP_BOARD_ADDENDUM_SHA256 = "78eb9c363217c455ef4b485a970167c8d0a93cfbbb61c9a6b889052cb93c771e"
REPLACEMENT_RTSP_MANIFEST_SHA256 = "3b20cc79d787357aa1de00484390b77cc81378a261763793d7a7a4e7bbd27c2c"
REPLACEMENT_RTSP_ELF_SHA256 = "7ae22105465167c4cd8223cdd588958e447ad7400e9979a0d55052d77784f685"
RETIRED_RTSP_MANIFEST_SHA256 = "65cceba353f1712ca56dc791d33ccde64c242d1e02a6fb05d7fb24d8fde087d4"
RETIRED_RTSP_ELF_SHA256 = "e7d4b0a36ee71f11c6589c66c8a9e21f055338e1c01490808ce00b11b0dd7553"

ALLOWED = {
    "CMakeLists.txt",
    "src/imu_reader_demo.cpp",
    "src/cam_demo.cpp",
    "src/cam_demo_common.h",
    "src/cam_demo_common.cpp",
    "src/cam_demo_config.h",
    "src/cam_demo_config.cpp",
    "src/cam_demo_pipeline.h",
    "src/cam_demo_pipeline.cpp",
    "src/cam_demo_rtsp.h",
    "src/cam_demo_rtsp.cpp",
    "scripts/build_cam_demo.sh",
    "scripts/build_imu_reader_demo.sh",
    "scripts/build_serial_port_demo.sh",
    "scripts/package_runtime.sh",
    "scripts/cam_demo_regression.sh",
    "scripts/verify_v2_modules.py",
    "scripts/verify_runtime_package.py",
    "tests/release008_fake_producers.h",
    "tests/release008_fake_producers.cpp",
    "tests/release008_consumer_lifecycle_test.cpp",
    "tests/release008_contract_test.py",
    "README.md",
    "README_EN.md",
    "RELEASE_CHECKLIST.md",
    "include/icm42688_driver.h",
    "include/sc132camera.h",
    "include/pr_venc.h",
    "lib/libicm42688.so",
    "lib/libsc132.so",
    "lib/libprrtsp.so",
    "demo/bin/cam_demo",
    "demo/bin/imu_reader_demo",
    "demo/bin/serial_port_demo",
    "demo/cam_demo",
    "demo/imu_reader_demo",
    "demo/serial_port_demo",
    "demo/env.sh",
    "demo/lib/libicm42688.so",
    "demo/lib/libsc132.so",
    "demo/lib/libprrtsp.so",
}
DELETED = {
    "include/icm42688_driver.h",
    "include/sc132camera.h",
    "include/pr_venc.h",
    "lib/libicm42688.so",
    "lib/libsc132.so",
    "lib/libprrtsp.so",
    "demo/bin/cam_demo",
    "demo/bin/imu_reader_demo",
    "demo/bin/serial_port_demo",
    "demo/cam_demo",
    "demo/imu_reader_demo",
    "demo/serial_port_demo",
    "demo/env.sh",
    "demo/lib/libicm42688.so",
    "demo/lib/libsc132.so",
    "demo/lib/libprrtsp.so",
}
PRODUCTION_TEXT = [
    "CMakeLists.txt",
    "src/imu_reader_demo.cpp",
    "src/cam_demo.cpp",
    "src/cam_demo_common.h",
    "src/cam_demo_common.cpp",
    "src/cam_demo_config.cpp",
    "src/cam_demo_pipeline.h",
    "src/cam_demo_pipeline.cpp",
    "src/cam_demo_rtsp.h",
    "src/cam_demo_rtsp.cpp",
    "scripts/package_runtime.sh",
]


def run(command: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None,
        expect: int = 0) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != expect:
        raise RuntimeError(f"command returned {result.returncode}, expected {expect}: {' '.join(command)}")
    return result


def git_lines(*args: str) -> set[str]:
    result = subprocess.run(["git", *args], cwd=ROOT, check=True, text=True,
                            stdout=subprocess.PIPE)
    return {line for line in result.stdout.splitlines() if line}


def validate_scope() -> None:
    changed = git_lines("diff", "--name-only")
    untracked = git_lines("ls-files", "--others", "--exclude-standard")
    staged = git_lines("diff", "--cached", "--name-only")
    touched = changed | untracked
    outside = touched - ALLOWED
    if outside:
        raise AssertionError(f"paths outside non_ros.allowed_paths: {sorted(outside)}")
    if staged:
        raise AssertionError(f"git index must remain untouched: {sorted(staged)}")
    deleted = git_lines("ls-files", "--deleted")
    if deleted != DELETED:
        raise AssertionError(f"tracked deletion mismatch: got={sorted(deleted)} expected={sorted(DELETED)}")


def source_map() -> dict[str, str]:
    return {name: (ROOT / name).read_text(encoding="utf-8") for name in PRODUCTION_TEXT}


def validate_source_contract(texts: dict[str, str]) -> None:
    combined = "\n".join(texts.values())
    forbidden = [
        "VioCamInit", "VioCamClose", "VioCamSetFps", "VioCamSetOutputRotate",
        "hb_mem_module_open", "hb_mem_module_close", "hb_mem_mgr.h",
        "Rtsp_SendImg_ch", "init_rtsp_ch", "rtspClose_ch",
        "--allow-shlib-undefined", "IMPORTED_NO_SONAME",
    ]
    present = [token for token in forbidden if token in combined]
    if present:
        raise AssertionError(f"retired/direct/local tokens remain: {present}")

    required_by_file = {
        "CMakeLists.txt": [
            "libicm42688.so.2.0.0", "libsc132.so.2.0.0", "libprrtsp.so.2.0.0",
            "sc132_v2", "prrtsp_v2", "-static-libstdc++", "$ORIGIN",
        ],
        "src/imu_reader_demo.cpp": [
            "icm42688_config_t", "icm42688_create", "icm42688_set_callback",
            "icm42688_start", "icm42688_stop", "icm42688_destroy", "noexcept",
        ],
        "src/cam_demo.cpp": [
            "sc132_set_fps", "sc132_set_output_rotation", "sc132_start_frame_set",
            "FinishSc132Shutdown", "CaptureStatuses", "CloseReverse",
        ],
        "src/cam_demo_pipeline.cpp": [
            "SC132_FRAME_SET_CONFIG_INIT", "sc132_frame_get_info", "sc132_frame_retain",
            "sc132_request_stop", "StopAndDrain", "FrameSetCallback", "noexcept",
            "sc132_stop();", "TotalSentFrames",
        ],
        "src/cam_demo_rtsp.cpp": [
            "PRRTSP_STREAM_CONFIG_V2_0_SIZE", "operation_timeout_ms = 1000U",
            "PRRTSP_NV12_FRAME_V2_0_SIZE", "y_physical_address = 0U",
            "uv_physical_address = 0U", "prrtsp_stream_get_status",
            "prrtsp_stream_close", "kCloseAttemptLimit = 3",
        ],
        "scripts/package_runtime.sh": [
            "libicm42688.so.2.0.0", "libsc132.so.2.0.0", "libprrtsp.so.2.0.0",
            "--strip-unneeded", "verify_runtime_package.py",
        ],
    }
    for name, tokens in required_by_file.items():
        missing = [token for token in tokens if token not in texts[name]]
        if missing:
            raise AssertionError(f"{name} misses contract tokens: {missing}")

    pipeline = texts["src/cam_demo_pipeline.cpp"]
    if pipeline.count("sc132_stop();") != 2:
        raise AssertionError("production lifecycle owner must contain exactly two explicit sc132_stop calls")
    callback_signature = ("void FramePipeline::FrameSetCallback(const sc132_frame_set_t* frame_set, "
                          "void* user) noexcept")
    if callback_signature not in pipeline:
        raise AssertionError("SC callback trampoline is not a noexcept exception firewall")


def validate_mutants(texts: dict[str, str]) -> None:
    mutants: list[tuple[str, str, str]] = [
        ("SC callback noexcept", "src/cam_demo_pipeline.cpp",
         "void FramePipeline::FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) noexcept"),
        ("RTSP timeout", "src/cam_demo_rtsp.cpp", "operation_timeout_ms = 1000U"),
        ("SC exact-two stop", "src/cam_demo_pipeline.cpp", "  sc132_stop();\n"),
        ("real ICM TU binding", "src/imu_reader_demo.cpp", "icm42688_create"),
    ]
    for label, name, token in mutants:
        mutated = dict(texts)
        if token not in mutated[name]:
            raise AssertionError(f"cannot construct mutant {label}")
        mutated[name] = mutated[name].replace(token, "/* RELEASE008_MUTANT */", 1)
        try:
            validate_source_contract(mutated)
        except AssertionError:
            continue
        raise AssertionError(f"contract validator accepted mutant: {label}")


def load_python_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Python module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path: Path, data: dict[str, object]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def verify_sc132_startup_correction_binding() -> None:
    """Bind production validation and packaging to the four-camera startup correction."""
    module_path = ROOT / "scripts/verify_v2_modules.py"
    package_path = ROOT / "scripts/package_runtime.sh"
    module_text = module_path.read_text(encoding="utf-8")
    package_text = package_path.read_text(encoding="utf-8")

    # production preflight 只接受满足四路 startup 契约的 SC132 producer。
    for token in (REPLACEMENT_SC_MANIFEST_SHA256, REPLACEMENT_SC_ELF_SHA256):
        if token not in module_text or token not in package_text:
            raise AssertionError(f"replacement SC132 identity missing from scripts: {token}")
    for token in (RETIRED_SC_MANIFEST_SHA256, RETIRED_SC_ELF_SHA256):
        if token in module_text or token in package_text:
            raise AssertionError(f"retired SC132 identity remains active in scripts: {token}")
    for prefix in STIPULATED_RETIRED_SC_PREFIXES:
        if prefix in module_text or prefix in package_text:
            raise AssertionError(f"stipulated retired SC132 identity remains active: {prefix}")

    module_info = load_python_module("release008_modules_sc132_startup", module_path)
    sc_spec = module_info.SPECS["sc132"]
    if sc_spec["manifest_sha256"] != REPLACEMENT_SC_MANIFEST_SHA256:
        raise AssertionError("module verifier does not bind replacement SC132 manifest")
    if sc_spec["artifacts"]["lib/libsc132.so.2.0.0"][1] != REPLACEMENT_SC_ELF_SHA256:
        raise AssertionError("module verifier does not bind replacement SC132 ELF")
    manifest = json.loads((SC_ROOT / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("identity", {}).get("build_id") != REPLACEMENT_SC_BUILD_ID:
        raise AssertionError("replacement SC132 Build ID provenance mismatch")


def verify_rtsp_board_addendum_binding() -> None:
    """Bind scripts and package metadata to the board-corrected RTSP operand."""
    module_path = ROOT / "scripts/verify_v2_modules.py"
    package_path = ROOT / "scripts/package_runtime.sh"
    runtime_path = ROOT / "scripts/verify_runtime_package.py"
    module_text = module_path.read_text(encoding="utf-8")
    package_text = package_path.read_text(encoding="utf-8")
    runtime_text = runtime_path.read_text(encoding="utf-8")

    # active package/module 只接受满足 RC 契约的 RTSP producer。
    for token in (REPLACEMENT_RTSP_MANIFEST_SHA256, REPLACEMENT_RTSP_ELF_SHA256):
        if token not in module_text or token not in package_text:
            raise AssertionError(f"replacement RTSP identity missing from scripts: {token}")
    for token in (RETIRED_RTSP_MANIFEST_SHA256, RETIRED_RTSP_ELF_SHA256):
        if token in module_text or token in package_text:
            raise AssertionError(f"retired RTSP identity remains active in scripts: {token}")
    field_literal = f'"rtsp_board_addendum_sha256": "{RTSP_BOARD_ADDENDUM_SHA256}"'
    if package_text.count(field_literal) != 2:
        raise AssertionError("runtime manifest and PACKAGE-COMPLETE must both bind exact RTSP addendum")
    for token in ("RTSP_BOARD_ADDENDUM_SHA256", "rtsp_board_addendum_sha256"):
        if token not in runtime_text:
            raise AssertionError(f"runtime verifier addendum gate missing: {token}")

    module_info = load_python_module("release008_modules_addendum", module_path)
    runtime_info = load_python_module("release008_runtime_addendum", runtime_path)
    if module_info.SPECS["prrtsp"]["manifest_sha256"] != REPLACEMENT_RTSP_MANIFEST_SHA256:
        raise AssertionError("module verifier does not bind replacement RTSP manifest")
    if module_info.SPECS["prrtsp"]["artifacts"]["lib/libprrtsp.so.2.0.0"][1] != REPLACEMENT_RTSP_ELF_SHA256:
        raise AssertionError("module verifier does not bind replacement RTSP ELF")

    with tempfile.TemporaryDirectory(prefix="RELEASE-008-addendum-field-") as temp:
        root = Path(temp)
        (root / "bin").mkdir()
        for name in runtime_info.EXPECTED_NEEDED:
            (root / "bin" / name).write_bytes(f"{name}-fixture".encode("ascii"))
        modules = {}
        for name in runtime_info.MODULE_NAMES:
            spec = module_info.SPECS[name]
            modules[name] = {
                "path": f"modules/{name}",
                "manifest_sha256": spec["manifest_sha256"],
                "real_elf_sha256": spec["artifacts"][spec["real"]][1],
            }
        runtime_data = {
            "schema": "RELEASE-008-non-ros-runtime-v1",
            "release_id": "4CAM-REL-20260714",
            "slice": "RELEASE-008/non_ros",
            "contract_sha256": runtime_info.CONTRACT_SHA256,
            "rtsp_board_addendum_sha256": RTSP_BOARD_ADDENDUM_SHA256,
            "producer_strip_count": 0,
            "binaries": {
                name: {
                    "path": f"bin/{name}",
                    "sha256": hashlib.sha256((root / "bin" / name).read_bytes()).hexdigest(),
                    "strip_policy": "consumer-only",
                }
                for name in runtime_info.EXPECTED_NEEDED
            },
            "modules": modules,
            "exact_direct_needed": runtime_info.EXPECTED_NEEDED,
            "build": {
                "build_dir": "/tmp/release008-build-fixture",
                "command": "cmake --build fixture",
                "toolchain_file": "/tmp/release008-toolchain-fixture.cmake",
            },
        }
        runtime_manifest = root / "RELEASE-008-runtime-manifest.json"
        write_json(runtime_manifest, runtime_data)
        runtime_info.verify_runtime_manifest(
            root, runtime_info.EXPECTED_NEEDED, module_info
        )
        for mutation in ("missing", "wrong"):
            mutant = dict(runtime_data)
            if mutation == "missing":
                mutant.pop("rtsp_board_addendum_sha256")
            else:
                mutant["rtsp_board_addendum_sha256"] = "0" * 64
            write_json(runtime_manifest, mutant)
            try:
                runtime_info.verify_runtime_manifest(
                    root, runtime_info.EXPECTED_NEEDED, module_info
                )
            except AssertionError:
                continue
            raise AssertionError(f"runtime verifier accepted {mutation} RTSP addendum")
        write_json(runtime_manifest, runtime_data)

        (root / "SHA256SUMS").write_text("fixture\n", encoding="utf-8")
        complete = {
            "schema": "RELEASE-008-package-complete-v1",
            "release_id": "4CAM-REL-20260714",
            "slice": "non_ros",
            "status": "complete",
            "contract_sha256": runtime_info.CONTRACT_SHA256,
            "rtsp_board_addendum_sha256": RTSP_BOARD_ADDENDUM_SHA256,
            "runtime_manifest": "RELEASE-008-runtime-manifest.json",
            "runtime_manifest_sha256": runtime_info.sha256(runtime_manifest),
            "checksums": "SHA256SUMS",
            "checksums_sha256": runtime_info.sha256(root / "SHA256SUMS"),
            "producer_strip_count": 0,
        }
        complete_path = root / "PACKAGE-COMPLETE.json"
        write_json(complete_path, complete)
        runtime_info.verify_complete(root)
        for mutation in ("missing", "wrong"):
            mutant = dict(complete)
            if mutation == "missing":
                mutant.pop("rtsp_board_addendum_sha256")
            else:
                mutant["rtsp_board_addendum_sha256"] = "0" * 64
            write_json(complete_path, mutant)
            try:
                runtime_info.verify_complete(root)
            except AssertionError:
                continue
            raise AssertionError(f"PACKAGE-COMPLETE verifier accepted {mutation} RTSP addendum")


def compiler() -> str:
    cxx = os.environ.get("CXX", "g++")
    path = shutil.which(cxx)
    if path is None:
        raise RuntimeError(f"missing host C++ compiler: {cxx}")
    return path


def compile_binary(output: Path, sanitizer: str | None,
                   cam_main_source: Path = ROOT / "src/cam_demo.cpp") -> None:
    common = [
        "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
        "-DRELEASE008_TESTING=1", "-I", str(ROOT / "src"), "-I", str(ROOT / "tests"),
        "-I", str(ROOT / "include"),
        "-I", str(ICM_ROOT / "include"), "-I", str(SC_ROOT / "include"),
        "-I", str(RTSP_ROOT / "include"),
    ]
    if sanitizer:
        common[0:0] = [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer", "-O1", "-g"]
    else:
        common[0:0] = ["-O2"]

    # 重命名真实 cam_demo main 后链接 harness，
    # 使 join-failure gate 执行 production cleanup owner。
    main_object = output.with_name(output.name + "-cam-main.o")
    run([compiler(), *common, "-Dmain=release008_cam_demo_main", "-c",
         str(cam_main_source), "-o", str(main_object)])
    flags = [
        compiler(), *common,
        str(ROOT / "src/cam_demo_common.cpp"),
        str(ROOT / "src/cam_demo_config.cpp"),
        str(ROOT / "src/cam_demo_rtsp.cpp"),
        str(ROOT / "src/cam_demo_pipeline.cpp"),
        str(ROOT / "src/imu_reader_demo.cpp"),
        str(ROOT / "tests/release008_fake_producers.cpp"),
        str(ROOT / "tests/release008_consumer_lifecycle_test.cpp"),
        str(main_object),
        "-o", str(output),
    ]
    run(flags)


def verify_module_gates() -> None:
    command = [
        sys.executable, str(ROOT / "scripts/verify_v2_modules.py"),
        "--icm", str(ICM_ROOT), "--sc132", str(SC_ROOT), "--prrtsp", str(RTSP_ROOT),
    ]
    run(command)
    retired_sc_command = list(command)
    retired_sc_command[retired_sc_command.index("--sc132") + 1] = str(RETIRED_SC_ROOT)
    run(retired_sc_command, expect=1)
    retired_command = list(command)
    retired_command[retired_command.index("--prrtsp") + 1] = str(RETIRED_RTSP_ROOT)
    run(retired_command, expect=1)
    with tempfile.TemporaryDirectory(prefix="RELEASE-008-module-mutant-") as temp:
        mutant = Path(temp) / "icm42688"
        shutil.copytree(ICM_ROOT, mutant, symlinks=True)
        header = mutant / "include/icm42688_x5/icm42688_driver.h"
        header.write_bytes(header.read_bytes() + b"\n/* mutant */\n")
        mutant_command = list(command)
        mutant_command[mutant_command.index("--icm") + 1] = str(mutant)
        run(mutant_command, expect=1)
    for kind in ("empty-dir", "fifo"):
        with tempfile.TemporaryDirectory(prefix=f"RELEASE-008-module-{kind}-") as temp:
            mutant = Path(temp) / "icm42688"
            shutil.copytree(ICM_ROOT, mutant, symlinks=True)
            if kind == "empty-dir":
                (mutant / "unrecorded-empty").mkdir()
            else:
                os.mkfifo(mutant / "unrecorded-fifo")
            mutant_command = list(command)
            mutant_command[mutant_command.index("--icm") + 1] = str(mutant)
            run(mutant_command, expect=1)


def verify_runtime_special_entry_gate() -> None:
    script = ROOT / "scripts/verify_runtime_package.py"
    runtime_text = script.read_text(encoding="utf-8")
    module_text = (ROOT / "scripts/verify_v2_modules.py").read_text(encoding="utf-8")
    required = {
        "runtime": (runtime_text, (
            "os.lstat", "stat.S_ISDIR", "stat.S_ISREG", "stat.S_ISLNK",
            "actual_inventory = verify_exact_inventory(root, expected_nodes)",
            "verify_checksums(root, actual_inventory)",
        )),
        "module": (module_text, (
            "os.lstat", "stat.S_ISDIR", "stat.S_ISREG", "stat.S_ISLNK",
            "verify_exact_inventory(root, expected_nodes, name)",
        )),
    }
    for label, (text, tokens) in required.items():
        missing = [token for token in tokens if token not in text]
        if missing:
            raise AssertionError(f"{label} special-entry verifier wiring missing: {missing}")
    spec = importlib.util.spec_from_file_location("release008_runtime_inventory", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load runtime package verifier")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for kind in ("empty-dir", "fifo"):
        with tempfile.TemporaryDirectory(prefix=f"RELEASE-008-runtime-{kind}-") as temp:
            root = Path(temp)
            (root / "payload").write_bytes(b"release008")
            if kind == "empty-dir":
                (root / "unrecorded-empty").mkdir()
            else:
                os.mkfifo(root / "unrecorded-fifo")
            try:
                module.verify_exact_inventory(root, {"payload": "regular"})
            except AssertionError:
                continue
            raise AssertionError(f"runtime verifier accepted unrecorded special entry: {kind}")


def run_main_behavior_mutants(build: Path) -> None:
    source = (ROOT / "src/cam_demo.cpp").read_text(encoding="utf-8")
    mutants = (
        ("exit-to-return", "    std::_Exit(1);", "    return 1;",
         "production cam_demo main returned after join failure"),
        ("close-before-exit", "    std::_Exit(1);",
         "    (void)rtsp->CaptureStatuses();\n"
         "    (void)rtsp->CloseReverse();\n"
         "    std::_Exit(1);",
         "join-failure main called RTSP status"),
    )
    for label, old, new, kill_evidence in mutants:
        if source.count(old) != 1:
            raise AssertionError(f"cannot construct behavior mutant {label}")
        mutant_source = build / f"cam_demo-{label}.cpp"
        mutant_source.write_text(source.replace(old, new, 1), encoding="utf-8")
        mutant_binary = build / f"lifecycle-mutant-{label}"
        compile_binary(mutant_binary, None, mutant_source)
        result = run([str(mutant_binary), "--main-join-gate-only"], expect=1)
        if kill_evidence not in result.stdout:
            raise AssertionError(f"behavior mutant {label} failed without expected kill evidence")


def run_lifecycle_gates(repeat: int) -> None:
    with tempfile.TemporaryDirectory(prefix="RELEASE-008-nonros-host-") as temp:
        build = Path(temp)
        strict = build / "lifecycle-strict"
        asan = build / "lifecycle-asan-ubsan"
        tsan = build / "lifecycle-tsan"
        compile_binary(strict, None)
        run([str(strict), "--repeat", str(repeat)])
        run_main_behavior_mutants(build)
        compile_binary(asan, "address,undefined")
        asan_env = dict(os.environ)
        asan_env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:strict_string_checks=1"
        asan_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        run([str(asan), "--repeat", "2"], env=asan_env)
        compile_binary(tsan, "thread")
        tsan_env = dict(os.environ)
        tsan_env["TSAN_OPTIONS"] = "halt_on_error=1:second_deadlock_stack=1"
        run([str(tsan), "--repeat", "2"], env=tsan_env)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeat", type=int, default=50)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    for root in (ICM_ROOT, SC_ROOT, RTSP_ROOT):
        if not root.is_dir():
            raise RuntimeError(f"missing producer module root: {root}")
    texts = source_map()
    validate_source_contract(texts)
    validate_mutants(texts)
    run_lifecycle_gates(args.repeat)
    print(f"RELEASE-008 non-ROS host contract PASS repeat={args.repeat}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - gate must print one deterministic failure
        print(f"RELEASE-008 non-ROS host contract FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
