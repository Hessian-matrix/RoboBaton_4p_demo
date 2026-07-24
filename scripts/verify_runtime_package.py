#!/usr/bin/env python3
"""Generate and verify a self-consistent non-ROS runtime package."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import stat
import subprocess
import sys

MANIFEST_NAME = "manifest.sha256"
EXPECTED_VERSION_NEEDS = {
    "bin/cam_demo": {"LIBSC132_2.0", "LIBPRRTSP_2.0"},
    "bin/imu_reader_demo": {"ICM42688_X5_2.0"},
    "bin/sensor_demo": {"ICM42688_X5_2.0", "LIBSC132_2.0", "LIBPRRTSP_2.0"},
}
EXPECTED_VERSION_DEFINITIONS = {
    "lib/libicm42688.so.2.0.0": "ICM42688_X5_2.0",
    "lib/libsc132.so.2.0.0": "LIBSC132_2.0",
    "lib/libprrtsp.so.2.0.0": "LIBPRRTSP_2.0",
}
EXPECTED_SONAMES = {
    "lib/libicm42688.so.2.0.0": "libicm42688.so.2",
    "lib/libsc132.so.2.0.0": "libsc132.so.2",
    "lib/libprrtsp.so.2.0.0": "libprrtsp.so.2",
}
EXPECTED_LIBRARY_COPIES = {
    "lib/libicm42688.so.2.0.0": {"lib/libicm42688.so.2", "lib/libicm42688.so"},
    "lib/libsc132.so.2.0.0": {"lib/libsc132.so.2", "lib/libsc132.so"},
    "lib/libprrtsp.so.2.0.0": {"lib/libprrtsp.so.2", "lib/libprrtsp.so"},
}
EXPECTED_NEEDED = {
    "bin/imu_reader_demo": {"libicm42688.so.2", "libm.so.6", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/sensor_demo": {"libicm42688.so.2", "libsc132.so.2", "libprrtsp.so.2", "libm.so.6", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/cam_demo": {"libsc132.so.2", "libprrtsp.so.2", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/serial_port_demo": {"libc.so.6", "ld-linux-aarch64.so.1"},
}
REQUIRED_FILES = {
    "cam_demo",
    "imu_reader_demo",
    "sensor_demo",
    "serial_port_demo",
    "env.sh",
    "bin/cam_demo",
    "bin/imu_reader_demo",
    "bin/sensor_demo",
    "bin/serial_port_demo",
    *EXPECTED_VERSION_DEFINITIONS,
    *(copy for copies in EXPECTED_LIBRARY_COPIES.values() for copy in copies),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def readelf(path: Path, *args: str) -> str:
    return subprocess.run(
        ["readelf", *args, str(path)], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True
    ).stdout


def version_names(path: Path) -> set[str]:
    return set(re.findall(r"Name:\s*([^\s]+)", readelf(path, "--version-info", "--wide")))


def soname(path: Path) -> str | None:
    match = re.search(r"Library soname: \[([^\]]+)\]", readelf(path, "-d"))
    return match.group(1) if match else None


def needed(path: Path) -> set[str]:
    result = set(re.findall(r"Shared library: \[([^\]]+)\]", readelf(path, "-d")))
    program = readelf(path, "-l")
    interpreter = re.search(r"Requesting program interpreter:\s*([^\]]+)\]", program)
    if interpreter:
        result.add(Path(interpreter.group(1)).name)
    return result


def verify_aarch64_executable(path: Path) -> None:
    header = readelf(path, "-h")
    if "Class:                             ELF64" not in header or \
            "Machine:                           AArch64" not in header:
        raise AssertionError(f"not an ELF64 AArch64 executable: {path}")
    program = readelf(path, "-l")
    if "/lib/ld-linux-aarch64.so.1" not in program:
        raise AssertionError(f"unexpected AArch64 interpreter: {path}")
    dynamic = readelf(path, "-d")
    for match in re.findall(r"Library (?:rpath|runpath): \[([^\]]+)\]", dynamic, re.I):
        if "/tmp/" in match or "/root/x5/" in match:
            raise AssertionError(f"absolute build RUNPATH in {path}: {match}")
        if "$ORIGIN" not in match:
            raise AssertionError(f"package-relative runtime lookup missing in {path}: {match}")


def package_files(package_dir: Path) -> list[Path]:
    return sorted(
        path for path in package_dir.rglob("*")
        if path.is_file() and path.name != MANIFEST_NAME
    )


def write_manifest(package_dir: Path) -> None:
    lines = [f"{sha256(path)}  {path.relative_to(package_dir).as_posix()}"
             for path in package_files(package_dir)]
    (package_dir / MANIFEST_NAME).write_text("\n".join(lines) + "\n")


def verify_manifest(package_dir: Path) -> None:
    manifest = package_dir / MANIFEST_NAME
    if not manifest.is_file():
        raise AssertionError(f"missing {MANIFEST_NAME}")
    entries: dict[str, str] = {}
    for line in manifest.read_text().splitlines():
        digest, separator, relative = line.partition("  ")
        if not separator or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise AssertionError(f"invalid manifest line: {line!r}")
        entries[relative] = digest
    actual_paths = {path.relative_to(package_dir).as_posix() for path in package_files(package_dir)}
    if set(entries) != actual_paths:
        raise AssertionError(
            f"manifest file set mismatch: missing={sorted(actual_paths - set(entries))}, "
            f"extra={sorted(set(entries) - actual_paths)}"
        )
    for relative, expected in entries.items():
        actual = sha256(package_dir / relative)
        if actual != expected:
            raise AssertionError(f"hash mismatch for {relative}: {actual} != {expected}")


def verify_package(package_dir: Path) -> None:
    missing = sorted(relative for relative in REQUIRED_FILES if not (package_dir / relative).is_file())
    if missing:
        raise AssertionError(f"missing runtime files: {missing}")

    for relative in ["cam_demo", "imu_reader_demo", "sensor_demo", "serial_port_demo", *EXPECTED_VERSION_NEEDS]:
        mode = (package_dir / relative).stat().st_mode
        if not mode & stat.S_IXUSR:
            raise AssertionError(f"not executable: {relative}")

    for relative, expected_needed in EXPECTED_NEEDED.items():
        executable = package_dir / relative
        verify_aarch64_executable(executable)
        actual_needed = needed(executable)
        if actual_needed != expected_needed:
            raise AssertionError(
                f"DT_NEEDED mismatch for {relative}: actual={sorted(actual_needed)} "
                f"expected={sorted(expected_needed)}"
            )

    for relative, expected_versions in EXPECTED_VERSION_NEEDS.items():
        versions = version_names(package_dir / relative)
        missing_versions = expected_versions - versions
        old_versions = {name for name in versions if name.endswith("_1.0") and
                        name.startswith(("ICM42688_", "LIBSC132_", "LIBPRRTSP_"))}
        if missing_versions or old_versions:
            raise AssertionError(
                f"ABI need mismatch for {relative}: missing={sorted(missing_versions)}, "
                f"old={sorted(old_versions)}"
            )

    for relative, expected_version in EXPECTED_VERSION_DEFINITIONS.items():
        path = package_dir / relative
        versions = version_names(path)
        if expected_version not in versions:
            raise AssertionError(f"{relative} does not define {expected_version}: {sorted(versions)}")
        actual_soname = soname(path)
        if actual_soname != EXPECTED_SONAMES[relative]:
            raise AssertionError(
                f"SONAME mismatch for {relative}: {actual_soname!r} != {EXPECTED_SONAMES[relative]!r}"
            )

    for real_relative, copies in EXPECTED_LIBRARY_COPIES.items():
        expected_hash = sha256(package_dir / real_relative)
        for copy_relative in copies:
            if sha256(package_dir / copy_relative) != expected_hash:
                raise AssertionError(f"producer copy drift: {copy_relative} != {real_relative}")

    verify_manifest(package_dir)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package_dir", type=Path)
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args()
    package_dir = args.package_dir.resolve()
    if args.write_manifest:
        write_manifest(package_dir)
    verify_package(package_dir)
    print(f"Runtime ABI package verified: {package_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f"Runtime ABI package verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
