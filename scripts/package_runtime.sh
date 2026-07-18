#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build_x5}"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_DIR}/demo}"
PACKAGE_LIB_DIR="${PACKAGE_LIB_DIR:-${PROJECT_DIR}/lib}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake}"
WORKSPACE_DIR="${WORKSPACE_DIR:-$(cd "${PROJECT_DIR}/../.." && pwd)}"
STRIP_TOOL="${STRIP_TOOL:-}"
WORK_ROOT=""
BACKUP_ACTIVE=0

usage() {
  cat <<'USAGE'
Usage:
  scripts/package_runtime.sh [options]

Default behavior:
  Recreate the CMake build directory, build every repository target from source,
  then publish a verified runtime package to ./demo.
  This rebuilds ICM42688, SC132, PRRTSP, and every non-ROS consumer target.

Options:
  --build-dir <path>       CMake build directory, default ./build_x5
  --output-dir <path>      Runtime package output directory, default ./demo
  --package-lib-dir <path> Runtime shared library source directory, default ./lib
  --toolchain-file <path>  CMake toolchain selecting the producer/consumer compiler
  --strip-tool <path>      Optional strip override for final staged executables only
  --workspace-dir <path>   Top-level workspace containing producer sources/scripts
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
    --workspace-dir)
      require_option_value "$1" "${2:-}"
      WORKSPACE_DIR="$2"
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

# 从 CMake 编译器元数据读取实际编译器，避免猜测 toolchain 文件中的条件分支。
read_configured_c_compiler() {
  local metadata_files=("${ICM_BUILD_DIR}"/CMakeFiles/*/CMakeCCompiler.cmake)
  local metadata_file=""
  local line=""
  local compiler=""

  if [[ "${#metadata_files[@]}" -ne 1 || ! -f "${metadata_files[0]}" ]]; then
    echo "Unable to locate unique CMake C compiler metadata under ${ICM_BUILD_DIR}" >&2
    exit 1
  fi
  metadata_file="${metadata_files[0]}"
  while IFS= read -r line; do
    if [[ "${line}" =~ ^set\(CMAKE_C_COMPILER\ \"([^\"]+)\"\)$ ]]; then
      compiler="${BASH_REMATCH[1]}"
      break
    fi
  done < "${metadata_file}"
  if [[ -z "${compiler}" ]]; then
    echo "Missing CMAKE_C_COMPILER in ${metadata_file}" >&2
    exit 1
  fi
  if [[ "${compiler}" != /* ]]; then
    compiler="$(command -v "${compiler}" || true)"
  fi
  if [[ -z "${compiler}" || ! -x "${compiler}" ]]; then
    echo "Configured C compiler is not executable: ${compiler:-<unresolved>}" >&2
    exit 1
  fi
  printf '%s\n' "${compiler}"
}

# 规范化路径后删除并重建 build 目录，确保发布不复用旧对象或旧 CMake 缓存。
BUILD_DIR="$(resolve_project_path "${BUILD_DIR}")"
OUTPUT_DIR="$(resolve_project_path "${OUTPUT_DIR}")"
PACKAGE_LIB_DIR="$(resolve_project_path "${PACKAGE_LIB_DIR}")"
TOOLCHAIN_FILE="$(resolve_project_path "${TOOLCHAIN_FILE}")"
WORKSPACE_DIR="$(resolve_project_path "${WORKSPACE_DIR}")"
mkdir -p "$(dirname "${BUILD_DIR}")" "$(dirname "${OUTPUT_DIR}")" "${PACKAGE_LIB_DIR}"
BUILD_DIR="$(cd "$(dirname "${BUILD_DIR}")" && pwd)/$(basename "${BUILD_DIR}")"
OUTPUT_DIR="$(cd "$(dirname "${OUTPUT_DIR}")" && pwd)/$(basename "${OUTPUT_DIR}")"
case "${BUILD_DIR}" in
  "${PROJECT_DIR}"/*) ;;
  *)
    echo "Refusing build directory outside project: ${BUILD_DIR}" >&2
    exit 1
    ;;
esac
if [[ "${BUILD_DIR}" == "${PROJECT_DIR}" || "${BUILD_DIR}" == "/" || -L "${BUILD_DIR}" ]]; then
  echo "Refusing unsafe build directory: ${BUILD_DIR}" >&2
  exit 1
fi

if [[ "${OUTPUT_DIR}" == "/" || "${OUTPUT_DIR}" == "${PROJECT_DIR}" ]]; then
  echo "Refusing unsafe output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi
if [[ -L "${OUTPUT_DIR}" ]]; then
  echo "Refusing symlink output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi
if [[ ! -f "${WORKSPACE_DIR}/CMakeLists.txt" || ! -x "${WORKSPACE_DIR}/scripts/build_sc132.sh" ||
      ! -x "${WORKSPACE_DIR}/scripts/build_rtsp_so_mp4.sh" ]]; then
  echo "Missing canonical producer workspace: ${WORKSPACE_DIR}" >&2
  exit 1
fi

# 先从权威源码干净构建三套 producer，并同步 SO/公共头到当前非 ROS 仓库。
ICM_BUILD_DIR="${PROJECT_DIR}/.package-build-icm42688"
rm -rf "${ICM_BUILD_DIR}"
cmake -S "${WORKSPACE_DIR}" -B "${ICM_BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DROBOBATON_SYNC_BUILT_SHARED_LIBS=ON \
  -DROBOBATON_DEMO_LIB_DIR="${PACKAGE_LIB_DIR}" \
  -DROBOBATON_DEMO_ICM_HEADER="${PROJECT_DIR}/include/icm42688_driver.h"
# 使用 CMake 实际选中的 C 编译器 triplet 统一 SC132 与 RTSP 的工具链。
PRODUCER_GCC="$(read_configured_c_compiler)"
if ! TARGET_TRIPLET="$("${PRODUCER_GCC}" -dumpmachine)"; then
  echo "Configured C compiler does not support -dumpmachine: ${PRODUCER_GCC}" >&2
  exit 1
fi
# 拒绝空值、路径或含空白的伪 triplet，避免拼接出仓库外或错误的工具路径。
if [[ -z "${TARGET_TRIPLET}" || "${TARGET_TRIPLET}" == */* || "${TARGET_TRIPLET}" == *[[:space:]]* ]]; then
  echo "Configured C compiler returned invalid target triplet: ${TARGET_TRIPLET:-<empty>}" >&2
  exit 1
fi
TOOLCHAIN_BIN_DIR="$(cd "$(dirname "${PRODUCER_GCC}")" && pwd)"
PRODUCER_CROSS_COMPILE="${TOOLCHAIN_BIN_DIR}/${TARGET_TRIPLET}-"
DERIVED_STRIP_TOOL="${PRODUCER_CROSS_COMPILE}strip"
# strip 必须与编译器位于同一目录并使用同一 triplet，保证 producer 和最终可执行文件使用同一工具链族。
if [[ ! -x "${DERIVED_STRIP_TOOL}" ]]; then
  echo "Missing companion strip for configured C compiler: ${DERIVED_STRIP_TOOL}" >&2
  exit 1
fi
if [[ -z "${STRIP_TOOL}" ]]; then
  STRIP_TOOL="${DERIVED_STRIP_TOOL}"
fi
if [[ ! -x "${STRIP_TOOL}" ]]; then
  echo "Strip tool is not executable: ${STRIP_TOOL}" >&2
  exit 1
fi

cmake --build "${ICM_BUILD_DIR}" --target icm42688_x5 --clean-first -j

PACKAGE_LIB_DIR="${PACKAGE_LIB_DIR}" \
  "${WORKSPACE_DIR}/scripts/build_sc132.sh" \
  --gcc "${PRODUCER_GCC}" --strip "${DERIVED_STRIP_TOOL}" --clean-first
PACKAGE_LIB_DIR="${PACKAGE_LIB_DIR}" PRRTSP_DEBUG=0 \
  "${WORKSPACE_DIR}/scripts/build_rtsp_so_mp4.sh" \
  --cross-compile "${PRODUCER_CROSS_COMPILE}" --clean-first


# 单次 configure 后构建默认 all 目标，覆盖仓库 CMakeLists 声明的全部可执行文件。
rm -rf "${BUILD_DIR}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
cmake --build "${BUILD_DIR}" --clean-first -j

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
