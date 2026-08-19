#!/usr/bin/env python3
"""Generate and verify a self-consistent, repo-bound non-ROS runtime package."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

MANIFEST_NAME = "manifest.sha256"
PROVENANCE_NAME = "runtime-provenance.json"
PROVENANCE_SCHEMA = "robobaton-non-ros-runtime-provenance-v2"
ROOT = Path(__file__).resolve().parents[1]
RELEASE_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

EXPECTED_VERSION_NEEDS = {
    "bin/cam_demo": {"LIBSC132_2.0", "LIBPRRTSP_2.0"},
    "bin/imu_reader_demo": {"ICM42688_X5_2.0", "ICM42688_X5_2.1"},
    "bin/sensor_demo": {"ICM42688_X5_2.0", "ICM42688_X5_2.1",
                        "LIBSC132_2.0", "LIBPRRTSP_2.0"},
}
EXPECTED_VERSION_DEFINITIONS = {
    "lib/libicm42688.so.2.1.0": {"ICM42688_X5_2.0", "ICM42688_X5_2.1"},
    "lib/libsc132.so.2.0.0": {"LIBSC132_2.0"},
    "lib/libprrtsp.so.2.0.0": {"LIBPRRTSP_2.0"},
}
EXPECTED_SONAMES = {
    "lib/libicm42688.so.2.1.0": "libicm42688.so.2",
    "lib/libsc132.so.2.0.0": "libsc132.so.2",
    "lib/libprrtsp.so.2.0.0": "libprrtsp.so.2",
}
EXPECTED_LIBRARY_COPIES = {
    "lib/libicm42688.so.2.1.0": {"lib/libicm42688.so.2", "lib/libicm42688.so"},
    "lib/libsc132.so.2.0.0": {"lib/libsc132.so.2", "lib/libsc132.so"},
    "lib/libprrtsp.so.2.0.0": {"lib/libprrtsp.so.2", "lib/libprrtsp.so"},
}
EXPECTED_SCRIPT_COPIES = {
    "scripts/runtime_ffprobe_frame_count.sh": "bin/ffprobe",
}
EXPECTED_NEEDED = {
    "bin/imu_reader_demo": {"libicm42688.so.2", "libm.so.6", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/sensor_demo": {"libicm42688.so.2", "libsc132.so.2", "libprrtsp.so.2", "libmultimedia.so.1", "libhbmem.so.1", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/cam_demo": {"libsc132.so.2", "libprrtsp.so.2", "libc.so.6", "ld-linux-aarch64.so.1"},
    "bin/serial_port_demo": {"libc.so.6", "ld-linux-aarch64.so.1"},
}
EXPECTED_LIBRARY_NEEDED = {
    "lib/libicm42688.so.2.1.0": {"libstdc++.so.6", "libm.so.6", "libgcc_s.so.1", "libc.so.6", "ld-linux-aarch64.so.1"},
    "lib/libsc132.so.2.0.0": {"libcam.so.1", "libvpf.so.1", "libhbmem.so.1", "libNano2D.so", "libc.so.6", "ld-linux-aarch64.so.1"},
    "lib/libprrtsp.so.2.0.0": {"libmultimedia.so.1", "libc.so.6", "ld-linux-aarch64.so.1"},
}
REQUIRED_FILES = {
    "VERSION",
    PROVENANCE_NAME,
    "cam_demo",
    "imu_reader_demo",
    "sensor_demo",
    "serial_port_demo",
    "env.sh",
    "config/sensor_config.yaml",
    "bin/cam_demo",
    "bin/imu_reader_demo",
    "bin/sensor_demo",
    "bin/serial_port_demo",
    "bin/ffprobe",
    *EXPECTED_VERSION_DEFINITIONS,
    *(copy for copies in EXPECTED_LIBRARY_COPIES.values() for copy in copies),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def readelf(path: Path, *args: str) -> str:
    return subprocess.run(
        ["readelf", *args, str(path)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        env={**os.environ, "LC_ALL": "C"},
    ).stdout


def version_names(path: Path) -> set[str]:
    return set(re.findall(r"Name:\s*([^\s]+)", readelf(path, "--version-info", "--wide")))


def soname(path: Path) -> str | None:
    match = re.search(r"Library soname: \[([^\]]+)\]", readelf(path, "-d"))
    return match.group(1) if match else None


def needed(path: Path) -> set[str]:
    return set(re.findall(r"Shared library: \[([^\]]+)\]", readelf(path, "-d")))


def dynamic_symbols(path: Path) -> set[str]:
    return set(
        re.findall(
            r"\b(?:GLOBAL|WEAK)\b\s+\bDEFAULT\b\s+\S+\s+([A-Za-z_][A-Za-z0-9_]*)",
            readelf(path, "--dyn-syms", "--wide"),
        )
    )


def verify_aarch64_executable(path: Path) -> None:
    header = readelf(path, "-h")
    elf_class = re.search(r"^\s*Class:\s+(\S+)\s*$", header, re.MULTILINE)
    machine = re.search(r"^\s*Machine:\s+(\S+)\s*$", header, re.MULTILINE)
    if not elf_class or elf_class.group(1) != "ELF64" or not machine or machine.group(1) != "AArch64":
        raise AssertionError(f"not an ELF64 AArch64 executable: {path}")
    program = readelf(path, "-l")
    interpreters = re.findall(r"Requesting program interpreter:\s*([^\]]+)\]", program)
    if interpreters != ["/lib/ld-linux-aarch64.so.1"]:
        raise AssertionError(f"unexpected AArch64 interpreter: {path}: {interpreters}")
    dynamic = readelf(path, "-d")
    for match in re.findall(r"Library (?:rpath|runpath): \[([^\]]+)\]", dynamic, re.I):
        if "/tmp/" in match or "/root/x5/" in match:
            raise AssertionError(f"absolute build RUNPATH in {path}: {match}")
        if "$ORIGIN" not in match:
            raise AssertionError(f"package-relative runtime lookup missing in {path}: {match}")


def git_output(repo_root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()

def git_bytes(repo_root: Path, *args: str) -> bytes:
    return subprocess.run(
        ["git", *args],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout


def workspace_context(repo_root: Path) -> tuple[Path, str]:
    repository_top = Path(git_output(repo_root, "rev-parse", "--show-toplevel")).resolve()
    if repository_top != repo_root.resolve():
        raise AssertionError(f"runtime source root is not a Git worktree root: {repo_root}")
    if repo_root.parent.name != "sub_module":
        raise AssertionError(f"runtime source is outside the required sub_module layout: {repo_root}")
    workspace_root = repo_root.parents[1].resolve()
    superproject_top = Path(git_output(workspace_root, "rev-parse", "--show-toplevel")).resolve()
    if superproject_top != workspace_root:
        raise AssertionError(f"runtime superproject root mismatch: {superproject_top} != {workspace_root}")
    return workspace_root, repo_root.relative_to(workspace_root).as_posix()


def git_status(repo_root: Path) -> str:
    return git_output(repo_root, "status", "--porcelain=v2", "-uall")


def committed_input_hashes(repo_root: Path, commit: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in repo_input_paths(repo_root):
        relative = path.relative_to(repo_root).as_posix()
        try:
            payload = git_bytes(repo_root, "cat-file", "blob", f"{commit}:{relative}")
        except subprocess.CalledProcessError as error:
            raise AssertionError(
                f"runtime provenance input is not committed at {commit}: {relative}"
            ) from error
        result[relative] = hashlib.sha256(payload).hexdigest()
    return result


def artifact_payload_hashes(package_dir: Path) -> dict[str, str]:
    excluded = {MANIFEST_NAME, PROVENANCE_NAME}
    return {
        path.relative_to(package_dir).as_posix(): sha256(path)
        for path in sorted(package_dir.rglob("*"))
        if path.is_file() and not path.is_symlink()
        and path.relative_to(package_dir).as_posix() not in excluded
    }


def repo_input_paths(repo_root: Path) -> list[Path]:
    paths = [
        repo_root / "VERSION",
        repo_root / "CMakeLists.txt",
        repo_root / "config/sensor_config.yaml",
        repo_root / "include/icm42688_driver.h",
        repo_root / "include/sc132camera.h",
        repo_root / "include/prrtsp_v2.h",
        repo_root / "scripts/package_runtime.sh",
        repo_root / "scripts/verify_runtime_package.py",
        repo_root / "scripts/runtime_ffprobe_frame_count.sh",
    ]
    paths.extend(sorted((repo_root / "src").glob("*")))
    paths.extend(sorted((repo_root / "lib").glob("lib*.so*")))
    return [path for path in paths if path.is_file()]


def repo_input_hashes(repo_root: Path) -> dict[str, str]:
    return {
        path.relative_to(repo_root).as_posix(): sha256(path)
        for path in repo_input_paths(repo_root)
    }


def expected_inventory_nodes() -> dict[str, str]:
    nodes: dict[str, str] = {
        "bin": "dir",
        "config": "dir",
        "lib": "dir",
        MANIFEST_NAME: "regular",
    }
    for relative in REQUIRED_FILES:
        nodes[relative] = "regular"
    return nodes


def verify_exact_inventory(root: Path, expected_nodes: dict[str, str]) -> dict[str, str]:
    actual_inventory: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            raise AssertionError(f"symlink is not allowed in runtime package: {relative}")
        if stat.S_ISDIR(st.st_mode):
            actual_inventory[relative] = "dir"
            continue
        if stat.S_ISREG(st.st_mode):
            actual_inventory[relative] = "regular"
            continue
        raise AssertionError(f"special file is not allowed in runtime package: {relative}")
    if actual_inventory != expected_nodes:
        missing = sorted(set(expected_nodes) - set(actual_inventory))
        extra = sorted(set(actual_inventory) - set(expected_nodes))
        raise AssertionError(f"runtime inventory mismatch: missing={missing}, extra={extra}")
    return actual_inventory


def manifest_regular_files(root: Path, actual_inventory: dict[str, str]) -> list[str]:
    return sorted(
        relative
        for relative, kind in actual_inventory.items()
        if kind == "regular" and relative != MANIFEST_NAME
    )


def write_atomic_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False, prefix=f".{path.name}."
    ) as handle:
        handle.write(content)
        temp_path = Path(handle.name)
    temp_path.replace(path)


def write_manifest(package_dir: Path) -> None:
    expected_nodes = expected_inventory_nodes()
    expected_nodes.pop(MANIFEST_NAME, None)
    actual_inventory = verify_exact_inventory(package_dir, expected_nodes)
    lines = [
        f"{sha256(package_dir / relative)}  {relative}"
        for relative in manifest_regular_files(package_dir, actual_inventory)
    ]
    write_atomic_text(package_dir / MANIFEST_NAME, "\n".join(lines) + "\n")


def verify_manifest(package_dir: Path) -> dict[str, str]:
    manifest = package_dir / MANIFEST_NAME
    if not manifest.is_file():
        raise AssertionError(f"missing {MANIFEST_NAME}")
    entries: dict[str, str] = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        digest, separator, relative = line.partition("  ")
        if not separator or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise AssertionError(f"invalid manifest line: {line!r}")
        if relative in entries:
            raise AssertionError(f"duplicate manifest path: {relative}")
        entries[relative] = digest
    return entries


def verify_checksums(root: Path, actual_inventory: dict[str, str]) -> None:
    entries = verify_manifest(root)
    regular_files = manifest_regular_files(root, actual_inventory)
    if sorted(entries) != regular_files:
        missing = sorted(set(regular_files) - set(entries))
        extra = sorted(set(entries) - set(regular_files))
        raise AssertionError(f"manifest file set mismatch: missing={missing}, extra={extra}")
    for relative, expected_hash in entries.items():
        actual_hash = sha256(root / relative)
        if actual_hash != expected_hash:
            raise AssertionError(f"hash mismatch for {relative}: {actual_hash} != {expected_hash}")


def capture_clean_source(repo_root: Path) -> tuple[dict[str, str], dict[str, object], dict[str, str]]:
    workspace_root, repository_path = workspace_context(repo_root)
    superproject_status = git_status(workspace_root)
    repository_status = git_status(repo_root)
    if superproject_status or repository_status:
        raise AssertionError(
            "runtime provenance requires clean source repositories before packaging: "
            f"superproject={superproject_status!r} runtime_source={repository_status!r}"
        )
    superproject_commit = git_output(workspace_root, "rev-parse", "HEAD")
    repository_commit = git_output(repo_root, "rev-parse", "HEAD")
    gitlink_commit = git_output(
        workspace_root, "rev-parse", f"{superproject_commit}:{repository_path}"
    )
    if gitlink_commit != repository_commit:
        raise AssertionError(
            "runtime source HEAD does not match the superproject gitlink: "
            f"repository={repository_commit} gitlink={gitlink_commit}"
        )
    inputs = committed_input_hashes(repo_root, repository_commit)
    if inputs != repo_input_hashes(repo_root):
        raise AssertionError("runtime source inputs differ from the recorded source commit")
    source = {
        "superproject_commit": superproject_commit,
        "repository_path": repository_path,
        "repository_commit": repository_commit,
        "superproject_gitlink_commit": gitlink_commit,
    }
    repository_states = {
        "superproject": {"commit": superproject_commit, "status": superproject_status},
        "runtime_source": {"commit": repository_commit, "status": repository_status},
    }
    return source, repository_states, inputs


def build_provenance(
    package_dir: Path,
    repo_root: Path,
    compiler: str,
    triplet: str,
    toolchain_file: Path,
    build_dir: Path,
) -> dict[str, object]:
    source, repository_states, inputs = capture_clean_source(repo_root)
    compiler_path = Path(compiler).resolve()
    toolchain_path = toolchain_file.resolve()
    if not compiler_path.is_file() or compiler_path.is_symlink():
        raise AssertionError(f"runtime provenance compiler is not a regular file: {compiler_path}")
    if not toolchain_path.is_file() or toolchain_path.is_symlink():
        raise AssertionError(f"runtime provenance toolchain is not a regular file: {toolchain_path}")
    return {
        "schema": PROVENANCE_SCHEMA,
        "release_version": RELEASE_VERSION,
        "source": source,
        "repository_states": repository_states,
        "artifact": {"files": artifact_payload_hashes(package_dir)},
        "toolchain": {
            "compiler": str(compiler_path),
            "compiler_sha256": sha256(compiler_path),
            "triplet": triplet,
            "toolchain_file": str(toolchain_path),
            "toolchain_file_sha256": sha256(toolchain_path),
            "build_dir": str(build_dir.resolve()),
        },
        "inputs": inputs,
    }


def write_provenance(
    package_dir: Path,
    repo_root: Path,
    compiler: str,
    triplet: str,
    toolchain_file: Path,
    build_dir: Path,
) -> None:
    provenance = build_provenance(
        package_dir, repo_root, compiler, triplet, toolchain_file, build_dir
    )
    write_atomic_text(
        package_dir / PROVENANCE_NAME,
        json.dumps(provenance, ensure_ascii=True, sort_keys=True, indent=2) + "\n",
    )


def verify_provenance(package_dir: Path, repo_root: Path) -> dict[str, object]:
    provenance_path = package_dir / PROVENANCE_NAME
    if not provenance_path.is_file():
        raise AssertionError(f"missing {PROVENANCE_NAME}")
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    if provenance.get("schema") != PROVENANCE_SCHEMA:
        raise AssertionError(f"unexpected provenance schema: {provenance.get('schema')!r}")
    if provenance.get("release_version") != RELEASE_VERSION:
        raise AssertionError("runtime provenance release_version drift")

    workspace_root, repository_path = workspace_context(repo_root)
    source = provenance.get("source")
    if not isinstance(source, dict) or source.get("repository_path") != repository_path:
        raise AssertionError("runtime provenance source identity is incomplete")
    superproject_commit = source.get("superproject_commit")
    repository_commit = source.get("repository_commit")
    gitlink_commit = source.get("superproject_gitlink_commit")
    for label, commit in (
        ("superproject", superproject_commit),
        ("runtime source", repository_commit),
        ("superproject gitlink", gitlink_commit),
    ):
        if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise AssertionError(f"runtime provenance {label} commit is invalid")
    try:
        git_output(workspace_root, "cat-file", "-e", f"{superproject_commit}^{{commit}}")
        git_output(repo_root, "cat-file", "-e", f"{repository_commit}^{{commit}}")
        committed_gitlink = git_output(
            workspace_root, "rev-parse", f"{superproject_commit}:{repository_path}"
        )
    except subprocess.CalledProcessError as error:
        raise AssertionError("runtime provenance source commit is unavailable") from error
    if committed_gitlink != repository_commit or gitlink_commit != repository_commit:
        raise AssertionError("runtime provenance source commit/gitlink binding mismatch")

    repository_states = provenance.get("repository_states")
    expected_states = {
        "superproject": {"commit": superproject_commit, "status": ""},
        "runtime_source": {"commit": repository_commit, "status": ""},
    }
    if repository_states != expected_states:
        raise AssertionError("runtime provenance requires recorded clean source repository states")

    stored_inputs = provenance.get("inputs")
    if not isinstance(stored_inputs, dict):
        raise AssertionError("runtime provenance inputs must be a dict")
    committed_inputs = committed_input_hashes(repo_root, repository_commit)
    if stored_inputs != committed_inputs:
        raise AssertionError("runtime provenance inputs do not match the recorded source commit")
    if stored_inputs != repo_input_hashes(repo_root):
        raise AssertionError("runtime provenance source/input hashes do not match current files")

    artifact = provenance.get("artifact")
    if not isinstance(artifact, dict) or artifact.get("files") != artifact_payload_hashes(package_dir):
        raise AssertionError("runtime provenance artifact payload hashes do not match package files")
    toolchain = provenance.get("toolchain")
    if not isinstance(toolchain, dict):
        raise AssertionError("runtime provenance toolchain block missing")
    for path_field, hash_field in (
        ("compiler", "compiler_sha256"),
        ("toolchain_file", "toolchain_file_sha256"),
    ):
        path_value = toolchain.get(path_field)
        digest = toolchain.get(hash_field)
        if not isinstance(path_value, str) or not isinstance(digest, str):
            raise AssertionError(f"runtime provenance missing toolchain field: {path_field}")
        path = Path(path_value)
        if not path.is_absolute() or not path.is_file() or path.is_symlink() or sha256(path) != digest:
            raise AssertionError(f"runtime provenance toolchain identity drift: {path_field}")
    for field in ("triplet", "build_dir"):
        if not toolchain.get(field):
            raise AssertionError(f"runtime provenance missing toolchain field: {field}")
    return provenance


def verify_package(package_dir: Path, *, repo_root: Path = ROOT) -> None:
    expected_nodes = expected_inventory_nodes()
    actual_inventory = verify_exact_inventory(package_dir, expected_nodes)
    verify_checksums(package_dir, actual_inventory)
    verify_provenance(package_dir, repo_root)

    missing = sorted(relative for relative in REQUIRED_FILES if not (package_dir / relative).is_file())
    if missing:
        raise AssertionError(f"missing runtime files: {missing}")
    if (package_dir / "VERSION").read_text(encoding="utf-8").strip() != RELEASE_VERSION:
        raise AssertionError("runtime VERSION does not match the release version")

    for relative in ["cam_demo", "imu_reader_demo", "sensor_demo", "serial_port_demo", "bin/ffprobe", *EXPECTED_VERSION_NEEDS]:
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

    for relative, expected_needed in EXPECTED_LIBRARY_NEEDED.items():
        actual_needed = needed(package_dir / relative)
        if actual_needed != expected_needed:
            raise AssertionError(
                f"producer DT_NEEDED mismatch for {relative}: actual={sorted(actual_needed)} "
                f"expected={sorted(expected_needed)}"
            )

    for relative, expected_versions in EXPECTED_VERSION_NEEDS.items():
        versions = version_names(package_dir / relative)
        missing_versions = expected_versions - versions
        old_versions = {
            name
            for name in versions
            if name.endswith("_1.0") and name.startswith(("ICM42688_", "LIBSC132_", "LIBPRRTSP_"))
        }
        if missing_versions or old_versions:
            raise AssertionError(
                f"ABI need mismatch for {relative}: missing={sorted(missing_versions)}, "
                f"old={sorted(old_versions)}"
            )

    for relative, expected_versions in EXPECTED_VERSION_DEFINITIONS.items():
        path = package_dir / relative
        versions = version_names(path)
        missing_versions = expected_versions - versions
        if missing_versions:
            raise AssertionError(
                f"{relative} does not define {sorted(missing_versions)}: {sorted(versions)}"
            )
        actual_soname = soname(path)
        if actual_soname != EXPECTED_SONAMES[relative]:
            raise AssertionError(
                f"SONAME mismatch for {relative}: {actual_soname!r} != {EXPECTED_SONAMES[relative]!r}"
            )

    version_getters = {
        "lib/libicm42688.so.2.1.0": "icm42688_get_version",
        "lib/libsc132.so.2.0.0": "sc132_get_version",
        "lib/libprrtsp.so.2.0.0": "prrtsp_get_version",
    }
    for relative, symbol in version_getters.items():
        if symbol not in dynamic_symbols(package_dir / relative):
            raise AssertionError(f"missing release version getter {symbol} in {relative}")

    for real_relative, copies in EXPECTED_LIBRARY_COPIES.items():
        expected_hash = sha256(package_dir / real_relative)
        for copy_relative in copies:
            if sha256(package_dir / copy_relative) != expected_hash:
                raise AssertionError(f"producer copy drift: {copy_relative} != {real_relative}")

    for source_relative, package_relative in EXPECTED_SCRIPT_COPIES.items():
        if sha256(repo_root / source_relative) != sha256(package_dir / package_relative):
            raise AssertionError(f"runtime script copy drift: {package_relative} != {source_relative}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package_dir", type=Path, nargs="?")
    parser.add_argument("--check-source", action="store_true")
    parser.add_argument("--write-manifest", action="store_true")
    parser.add_argument("--write-provenance", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--compiler", default="")
    parser.add_argument("--triplet", default="")
    parser.add_argument("--toolchain-file", type=Path)
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    if args.check_source:
        capture_clean_source(repo_root)
        print(f"Runtime source snapshot verified: {repo_root}")
        return 0
    if args.package_dir is None:
        raise SystemExit("package_dir is required unless --check-source is used")
    package_dir = args.package_dir.resolve()
    if args.write_provenance:
        if not args.compiler or not args.triplet or args.toolchain_file is None or args.build_dir is None:
            raise SystemExit("--write-provenance requires --compiler, --triplet, --toolchain-file, and --build-dir")
        write_provenance(
            package_dir,
            repo_root,
            args.compiler,
            args.triplet,
            args.toolchain_file.resolve(),
            args.build_dir.resolve(),
        )
    if args.write_manifest:
        write_manifest(package_dir)
    verify_package(package_dir, repo_root=repo_root)
    print(f"Runtime ABI package verified: {package_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f"Runtime ABI package verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
