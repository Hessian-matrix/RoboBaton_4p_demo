#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build_x5"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_DIR}/demo}"
PACKAGE_LIB_DIR="${PACKAGE_LIB_DIR:-${PROJECT_DIR}/lib}"
STRIP_TOOL="${STRIP_TOOL:-}"

usage() {
  cat <<'USAGE'
Usage:
  scripts/package_runtime.sh [options]

Options:
  --build-dir <path>       CMake build directory that contains demo binaries, default ./build_x5
  --output-dir <path>      Runtime package output directory, default ./demo
  --package-lib-dir <path> Runtime shared library source directory, default ./lib
  --strip-tool <path>      Strip tool, default auto-detect aarch64 strip
  -h, --help               Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --package-lib-dir|--runtime-lib-dir) PACKAGE_LIB_DIR="$2"; shift 2 ;;
    --strip-tool) STRIP_TOOL="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

resolve_project_path() {
  local path="$1"
  if [[ "${path}" = /* ]]; then
    printf '%s\n' "${path}"
  else
    printf '%s\n' "${PROJECT_DIR}/${path}"
  fi
}


# 2026-07-08 修改原因：相对 build 目录也按脚本所属工程解析，保证从仓库外调用 --build-dir build_x5 可复现。
BUILD_DIR="$(resolve_project_path "${BUILD_DIR}")"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
# 2026-07-08 修改原因：相对输出目录也按脚本所属工程解析，保证从仓库外调用 --output-dir demo 仍写入工程 demo。
OUTPUT_DIR="$(resolve_project_path "${OUTPUT_DIR}")"
mkdir -p "$(dirname "${OUTPUT_DIR}")"
OUTPUT_DIR="$(cd "$(dirname "${OUTPUT_DIR}")" && pwd)/$(basename "${OUTPUT_DIR}")"

# 2026-07-08 修改原因：运行包和构建 wrapper 使用同一个库源目录变量，避免 build 同步到自定义目录后打包仍读取旧 lib。
# 2026-07-08 修改原因：相对库目录按脚本所属工程解析，保证从仓库外调用 --package-lib-dir lib 仍指向工程 lib。
PACKAGE_LIB_DIR="$(resolve_project_path "${PACKAGE_LIB_DIR}")"
if [[ ! -d "${PACKAGE_LIB_DIR}" ]]; then
  echo "Missing runtime lib directory: ${PACKAGE_LIB_DIR}" >&2
  exit 1
fi
PACKAGE_LIB_DIR="$(cd "${PACKAGE_LIB_DIR}" && pwd)"

for path in \
  "${BUILD_DIR}/cam_demo" \
  "${BUILD_DIR}/imu_reader_demo" \
  "${BUILD_DIR}/serial_port_demo" \
  "${PACKAGE_LIB_DIR}/libicm42688.so.2.0.0" \
  "${PACKAGE_LIB_DIR}/libicm42688.so.2" \
  "${PACKAGE_LIB_DIR}/libicm42688.so" \
  "${PACKAGE_LIB_DIR}/libsc132.so.2.0.0" \
  "${PACKAGE_LIB_DIR}/libsc132.so.2" \
  "${PACKAGE_LIB_DIR}/libsc132.so" \
  "${PACKAGE_LIB_DIR}/libprrtsp.so.2.0.0" \
  "${PACKAGE_LIB_DIR}/libprrtsp.so.2" \
  "${PACKAGE_LIB_DIR}/libprrtsp.so"; do
  if [[ ! -f "${path}" ]]; then
    echo "Missing required file: ${path}" >&2
    exit 1
  fi
done

if [[ "${OUTPUT_DIR}" == "/" || "${OUTPUT_DIR}" == "${PROJECT_DIR}" ]]; then
  echo "Refusing unsafe output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/bin" "${OUTPUT_DIR}/lib"

cp "${BUILD_DIR}/cam_demo" "${OUTPUT_DIR}/bin/"
cp "${BUILD_DIR}/imu_reader_demo" "${OUTPUT_DIR}/bin/"
cp "${BUILD_DIR}/serial_port_demo" "${OUTPUT_DIR}/bin/"
# 2026-07-16 修改原因：保留 demo/lib 目录与 unversioned 入口，同时随 real ELF 提供 major-2 loader 名称。
for library in \
  libicm42688.so.2.0.0 libicm42688.so.2 libicm42688.so \
  libsc132.so.2.0.0 libsc132.so.2 libsc132.so \
  libprrtsp.so.2.0.0 libprrtsp.so.2 libprrtsp.so; do
  cp "${PACKAGE_LIB_DIR}/${library}" "${OUTPUT_DIR}/lib/${library}"
done

if [[ -z "${STRIP_TOOL}" ]]; then
  if command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
    STRIP_TOOL="$(command -v aarch64-linux-gnu-strip)"
  elif [[ -x /opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-strip ]]; then
    STRIP_TOOL="/opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-strip"
  fi
fi

if [[ -n "${STRIP_TOOL}" ]]; then
  "${STRIP_TOOL}" --strip-unneeded "${OUTPUT_DIR}/bin/cam_demo"
  "${STRIP_TOOL}" --strip-unneeded "${OUTPUT_DIR}/bin/imu_reader_demo"
  "${STRIP_TOOL}" --strip-unneeded "${OUTPUT_DIR}/bin/serial_port_demo"
fi

# Preserve the canonical wrapper/environment entrypoints byte-for-byte.
for entry in env.sh cam_demo imu_reader_demo serial_port_demo; do
  source_entry="${PROJECT_DIR}/demo/${entry}"
  if [[ ! -f "${source_entry}" ]]; then
    echo "Missing canonical runtime entrypoint: ${source_entry}" >&2
    exit 1
  fi
  cp "${source_entry}" "${OUTPUT_DIR}/${entry}"
done

chmod 755 "${OUTPUT_DIR}" "${OUTPUT_DIR}/bin" "${OUTPUT_DIR}/lib"
chmod 755 "${OUTPUT_DIR}/cam_demo" "${OUTPUT_DIR}/imu_reader_demo" "${OUTPUT_DIR}/serial_port_demo"
chmod 755 "${OUTPUT_DIR}/bin/cam_demo" "${OUTPUT_DIR}/bin/imu_reader_demo" "${OUTPUT_DIR}/bin/serial_port_demo"
chmod 644 "${OUTPUT_DIR}/env.sh" "${OUTPUT_DIR}/lib/"*.so

# 2026-07-14 修改原因：运行包必须绑定同一 ABI 批次并携带可复核哈希，禁止旧可执行文件混入 v2 SO。
python3 "${SCRIPT_DIR}/verify_runtime_package.py" --write-manifest "${OUTPUT_DIR}"
chmod 644 "${OUTPUT_DIR}/manifest.sha256"

echo "Runtime package generated: ${OUTPUT_DIR}"
