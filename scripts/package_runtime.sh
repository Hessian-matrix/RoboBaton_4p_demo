#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build_x5}"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_DIR}/demo}"
PACKAGE_LIB_DIR="${PACKAGE_LIB_DIR:-${PROJECT_DIR}/lib}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake}"
STRIP_TOOL="${STRIP_TOOL:-}"
WORK_ROOT=""
BACKUP_ACTIVE=0

usage() {
  cat <<'USAGE'
Usage:
  scripts/package_runtime.sh [options]

Default behavior:
  Build cam_demo, imu_reader_demo and serial_port_demo, then publish a verified
  runtime package to ./demo.

Options:
  --build-dir <path>       CMake build directory, default ./build_x5
  --output-dir <path>      Runtime package output directory, default ./demo
  --package-lib-dir <path> Runtime shared library source directory, default ./lib
  --toolchain-file <path>  CMake toolchain file used by all three builds
  --strip-tool <path>      Strip tool, default auto-detect aarch64 strip
  -h, --help               Show this help
USAGE
}

require_option_value() {
  local option="$1"
  local value="${2:-}"
  if [[ -z "${value}" ]]; then
    echo "Missing value for ${option}" >&2
    exit 2
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      require_option_value "$1" "${2:-}"
      BUILD_DIR="$2"
      shift 2
      ;;
    --output-dir)
      require_option_value "$1" "${2:-}"
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --package-lib-dir|--runtime-lib-dir)
      require_option_value "$1" "${2:-}"
      PACKAGE_LIB_DIR="$2"
      shift 2
      ;;
    --toolchain-file)
      require_option_value "$1" "${2:-}"
      TOOLCHAIN_FILE="$2"
      shift 2
      ;;
    --strip-tool)
      require_option_value "$1" "${2:-}"
      STRIP_TOOL="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
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

# 构建前规范化路径，使入口可创建尚不存在的 build 目录。
BUILD_DIR="$(resolve_project_path "${BUILD_DIR}")"
OUTPUT_DIR="$(resolve_project_path "${OUTPUT_DIR}")"
PACKAGE_LIB_DIR="$(resolve_project_path "${PACKAGE_LIB_DIR}")"
TOOLCHAIN_FILE="$(resolve_project_path "${TOOLCHAIN_FILE}")"
mkdir -p "${BUILD_DIR}" "$(dirname "${OUTPUT_DIR}")"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
OUTPUT_DIR="$(cd "$(dirname "${OUTPUT_DIR}")" && pwd)/$(basename "${OUTPUT_DIR}")"

if [[ ! -d "${PACKAGE_LIB_DIR}" ]]; then
  echo "Missing runtime lib directory: ${PACKAGE_LIB_DIR}" >&2
  exit 1
fi
PACKAGE_LIB_DIR="$(cd "${PACKAGE_LIB_DIR}" && pwd)"

if [[ "${OUTPUT_DIR}" == "/" || "${OUTPUT_DIR}" == "${PROJECT_DIR}" ]]; then
  echo "Refusing unsafe output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi
if [[ -L "${OUTPUT_DIR}" ]]; then
  echo "Refusing symlink output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi

# 三个 consumer 共用同一 build 目录和 toolchain。
for builder in \
  build_cam_demo.sh \
  build_imu_reader_demo.sh \
  build_serial_port_demo.sh; do
  BUILD_DIR="${BUILD_DIR}" TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    "${SCRIPT_DIR}/${builder}"
done

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

if [[ -z "${STRIP_TOOL}" ]]; then
  if command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
    STRIP_TOOL="$(command -v aarch64-linux-gnu-strip)"
  elif [[ -x /opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-strip ]]; then
    STRIP_TOOL="/opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-strip"
  fi
fi
if [[ -n "${STRIP_TOOL}" && ! -x "${STRIP_TOOL}" ]]; then
  echo "Strip tool is not executable: ${STRIP_TOOL}" >&2
  exit 1
fi

cleanup() {
  local rc=$?
  # 发布异常退出时恢复同一文件系统中的上一版。
  if [[ "${BACKUP_ACTIVE}" -eq 1 && ! -e "${OUTPUT_DIR}" && -e "${WORK_ROOT}/previous" ]]; then
    mv "${WORK_ROOT}/previous" "${OUTPUT_DIR}" || true
  fi
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
  exit "${rc}"
}
trap cleanup EXIT

# stage 完成 strip、manifest 和验证后再原子发布。
WORK_ROOT="$(mktemp -d "$(dirname "${OUTPUT_DIR}")/.package-runtime.XXXXXX")"
STAGE_DIR="${WORK_ROOT}/stage"
mkdir -p "${STAGE_DIR}/bin" "${STAGE_DIR}/lib"

cp "${BUILD_DIR}/cam_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/imu_reader_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/serial_port_demo" "${STAGE_DIR}/bin/"
for library in \
  libicm42688.so.2.0.0 libicm42688.so.2 libicm42688.so \
  libsc132.so.2.0.0 libsc132.so.2 libsc132.so \
  libprrtsp.so.2.0.0 libprrtsp.so.2 libprrtsp.so; do
  cp "${PACKAGE_LIB_DIR}/${library}" "${STAGE_DIR}/lib/${library}"
done

if [[ -n "${STRIP_TOOL}" ]]; then
  "${STRIP_TOOL}" --strip-unneeded "${STAGE_DIR}/bin/cam_demo"
  "${STRIP_TOOL}" --strip-unneeded "${STAGE_DIR}/bin/imu_reader_demo"
  "${STRIP_TOOL}" --strip-unneeded "${STAGE_DIR}/bin/serial_port_demo"
fi

cat > "${STAGE_DIR}/env.sh" <<'EOF'
#!/bin/sh
DEMO_DIR="${DEMO_DIR:-$(pwd)}"
DEMO_LD_LIBRARY_PATH="${DEMO_DIR}/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}:${LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}"
fi
unset DEMO_LD_LIBRARY_PATH
EOF

for name in cam_demo imu_reader_demo serial_port_demo; do
  cat > "${STAGE_DIR}/${name}" <<EOF
#!/bin/sh
set -eu
DEMO_DIR="\$(cd "\$(dirname "\$0")" && pwd)"
export DEMO_DIR
. "\${DEMO_DIR}/env.sh"
exec "\${DEMO_DIR}/bin/${name}" "\$@"
EOF
done

chmod 755 "${STAGE_DIR}" "${STAGE_DIR}/bin" "${STAGE_DIR}/lib"
chmod 755 "${STAGE_DIR}/cam_demo" "${STAGE_DIR}/imu_reader_demo" "${STAGE_DIR}/serial_port_demo"
chmod 755 "${STAGE_DIR}/bin/cam_demo" "${STAGE_DIR}/bin/imu_reader_demo" "${STAGE_DIR}/bin/serial_port_demo"
chmod 644 "${STAGE_DIR}/env.sh" "${STAGE_DIR}/lib/"*.so

python3 "${SCRIPT_DIR}/verify_runtime_package.py" --write-manifest "${STAGE_DIR}"
chmod 644 "${STAGE_DIR}/manifest.sha256"
python3 "${SCRIPT_DIR}/verify_runtime_package.py" "${STAGE_DIR}"

if [[ -e "${OUTPUT_DIR}" ]]; then
  mv "${OUTPUT_DIR}" "${WORK_ROOT}/previous"
  BACKUP_ACTIVE=1
fi
mv "${STAGE_DIR}" "${OUTPUT_DIR}"
BACKUP_ACTIVE=0
if [[ -e "${WORK_ROOT}/previous" ]]; then
  rm -rf "${WORK_ROOT}/previous"
fi

trap - EXIT
rm -rf "${WORK_ROOT}"
WORK_ROOT=""
echo "Runtime package generated and verified: ${OUTPUT_DIR}"
