#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${PROJECT_DIR}/build_x5/sensor_web"
CARGO_EXECUTABLE="${SENSOR_WEB_CARGO_EXECUTABLE:-cargo}"
usage() {
  cat <<'USAGE'
Usage: scripts/build_sensor_web.sh [--output <path>] [--cargo <executable>]

Build static Linux ARM64 sensor_web using Rust 1.95.0's bundled musl 1.2.5
mallocng and rust-lld. cargo-zigbuild 0.23.0 / Zig 0.16.0 compile native C
dependencies only. Provision the aarch64-unknown-linux-musl Rust target and
Cargo.lock dependencies before building. Builds are frozen/offline and never
install compilers or board tools; deterministic ELF inspection verifies output.
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output|--cargo)
      if [[ $# -lt 2 || -z "$2" ]]; then echo "Missing value for $1" >&2; exit 2; fi
      if [[ "$1" == --output ]]; then OUTPUT="$2"; else CARGO_EXECUTABLE="$2"; fi
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
CARGO_EXECUTABLE="$(command -v "${CARGO_EXECUTABLE}")"
if [[ "$(rustc --version)" != 'rustc 1.95.0 (59807616e 2026-04-14)' ]]; then
  echo 'sensor_web requires exactly Rust 1.95.0; provision it before building.' >&2; exit 1
fi
if ! cargo install --list | python3 -c 'import sys; sys.exit("cargo-zigbuild v0.23.0:" not in sys.stdin.read().splitlines())'; then
  echo 'sensor_web requires cargo-zigbuild 0.23.0; provision it before building.' >&2; exit 1
fi
if command -v zig >/dev/null; then ZIG_VERSION="$(zig version)"; else ZIG_VERSION="$(python3 -m ziglang version)"; fi
if [[ "$ZIG_VERSION" != 0.16.0 ]]; then echo 'sensor_web requires Zig 0.16.0' >&2; exit 1; fi
mkdir -p "$(dirname "${OUTPUT}")"
OUTPUT="$(cd "$(dirname "${OUTPUT}")" && pwd)/$(basename "${OUTPUT}")"
case "${OUTPUT}" in
  "${PROJECT_DIR}/web/"*|"${PROJECT_DIR}/src/"*|"${PROJECT_DIR}/scripts/"*)
    echo "Refusing sensor_web output inside source inputs: ${OUTPUT}" >&2; exit 1 ;;
esac
TARGET_DIR="$(dirname "${OUTPUT}")/build_rust_web"
TEMP_OUTPUT="$(mktemp "${OUTPUT}.XXXXXX")"
trap 'rm -f "${TEMP_OUTPUT}"' EXIT
# Final libc comes from the pinned Rust target, not Zig's retaining allocator.
RUST_SYSROOT="$(rustc --print sysroot)"
RUST_HOST="$(rustc --print host-tuple)"
RUST_LLD="${RUST_SYSROOT}/lib/rustlib/${RUST_HOST}/bin/rust-lld"
if [[ ! -x "${RUST_LLD}" ]]; then echo "Missing Rust linker: ${RUST_LLD}" >&2; exit 1; fi
export RUSTFLAGS="-C relocation-model=static -C target-feature=+crt-static -C link-self-contained=yes -C linker=${RUST_LLD} -C linker-flavor=ld.lld"
"${CARGO_EXECUTABLE}" zigbuild --frozen --release --bin sensor_web \
  --manifest-path "${PROJECT_DIR}/web/server/Cargo.toml" \
  --target aarch64-unknown-linux-musl --target-dir "${TARGET_DIR}"
cp "${TARGET_DIR}/aarch64-unknown-linux-musl/release/sensor_web" "${TEMP_OUTPUT}"
python3 "${SCRIPT_DIR}/verify_runtime_package.py" --sensor-web "${TEMP_OUTPUT}"
chmod 755 "${TEMP_OUTPUT}"
mv "${TEMP_OUTPUT}" "${OUTPUT}"
trap - EXIT
printf 'Built sensor_web: %s\n' "${OUTPUT}"
