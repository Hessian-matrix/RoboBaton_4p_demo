#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build_x5}"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_DIR}/demo}"
PACKAGE_LIB_DIR="${PROJECT_DIR}/lib"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake}"
STRIP_TOOL="${STRIP_TOOL:-}"
WORK_ROOT=""
BACKUP_ACTIVE=0

usage() {
  cat <<'USAGE'
Usage:
  scripts/package_runtime.sh [options]

Behavior:
  Build the five non-ROS consumer demos from this release repository, using the
  prebuilt runtime libraries already present in ./lib, then publish a verified
  runtime package to ./demo.

  This script does not build ICM42688, SC132, or PRRTSP producer sources.
  Producer libraries and the public ICM header must already be present in this
  release repository before this script is run.

Options:
  --build-dir <path>       CMake build directory, default ./build_x5
  --output-dir <path>      Runtime package output directory, default ./demo
  --toolchain-file <path>  CMake toolchain for the consumer demos
  --strip-tool <path>      Optional strip override for staged executables
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

read_configured_c_compiler() {
  local metadata_files=("${BUILD_DIR}"/CMakeFiles/*/CMakeCCompiler.cmake)
  local metadata_file=""
  local line=""
  local compiler=""

  if [[ "${#metadata_files[@]}" -ne 1 || ! -f "${metadata_files[0]}" ]]; then
    echo "Unable to locate unique CMake C compiler metadata under ${BUILD_DIR}" >&2
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

BUILD_DIR="$(resolve_project_path "${BUILD_DIR}")"
OUTPUT_DIR="$(resolve_project_path "${OUTPUT_DIR}")"
TOOLCHAIN_FILE="$(resolve_project_path "${TOOLCHAIN_FILE}")"

mkdir -p "$(dirname "${BUILD_DIR}")" "$(dirname "${OUTPUT_DIR}")"
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
if [[ "${OUTPUT_DIR}" == "/" || "${OUTPUT_DIR}" == "${PROJECT_DIR}" || -L "${OUTPUT_DIR}" ]]; then
  echo "Refusing unsafe output directory: ${OUTPUT_DIR}" >&2
  exit 1
fi

for library in \
  libicm42688.so.2.0.0 libicm42688.so.2 libicm42688.so \
  libsc132.so.2.0.0 libsc132.so.2 libsc132.so \
  libprrtsp.so.2.0.0 libprrtsp.so.2 libprrtsp.so; do
  if [[ ! -f "${PACKAGE_LIB_DIR}/${library}" ]]; then
    echo "Missing prebuilt release library: ${PACKAGE_LIB_DIR}/${library}" >&2
    exit 1
  fi
done
if [[ ! -f "${PROJECT_DIR}/include/icm42688_driver.h" ]]; then
  echo "Missing release public header: ${PROJECT_DIR}/include/icm42688_driver.h" >&2
  exit 1
fi
if [[ ! -f "${PROJECT_DIR}/config/sensor_config.yaml" ]]; then
  echo "Missing release sensor config: ${PROJECT_DIR}/config/sensor_config.yaml" >&2
  exit 1
fi
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "Missing consumer toolchain file: ${TOOLCHAIN_FILE}" >&2
  exit 1
fi

# Configure only this release repository. The imported producer libraries come from ./lib.
rm -rf "${BUILD_DIR}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCMAKE_BUILD_TYPE="Release"

PRODUCER_GCC="$(read_configured_c_compiler)"
if ! TARGET_TRIPLET="$("${PRODUCER_GCC}" -dumpmachine)"; then
  echo "Configured C compiler does not support -dumpmachine: ${PRODUCER_GCC}" >&2
  exit 1
fi
if [[ -z "${TARGET_TRIPLET}" || "${TARGET_TRIPLET}" == */* || "${TARGET_TRIPLET}" == *[[:space:]]* ]]; then
  echo "Configured C compiler returned invalid target triplet: ${TARGET_TRIPLET:-<empty>}" >&2
  exit 1
fi
TOOLCHAIN_BIN_DIR="$(cd "$(dirname "${PRODUCER_GCC}")" && pwd)"
DERIVED_STRIP_TOOL="${TOOLCHAIN_BIN_DIR}/${TARGET_TRIPLET}-strip"
if [[ -z "${STRIP_TOOL}" ]]; then
  STRIP_TOOL="${DERIVED_STRIP_TOOL}"
fi
if [[ ! -x "${STRIP_TOOL}" ]]; then
  if [[ "${STRIP_TOOL}" == "${DERIVED_STRIP_TOOL}" ]]; then
    echo "Missing companion strip for configured C compiler: ${STRIP_TOOL}" >&2
  else
    echo "Strip tool is not executable: ${STRIP_TOOL}" >&2
  fi
  exit 1
fi

cmake --build "${BUILD_DIR}" --clean-first -j

for path in \
  "${BUILD_DIR}/cam_demo" \
  "${BUILD_DIR}/imu_reader_demo" \
  "${BUILD_DIR}/sensor_demo" \
  "${BUILD_DIR}/mosaic_rtsp_demo" \
  "${BUILD_DIR}/serial_port_demo"; do
  if [[ ! -f "${path}" ]]; then
    echo "Missing required consumer executable: ${path}" >&2
    exit 1
  fi
done

cleanup() {
  local rc=$?
  if [[ "${BACKUP_ACTIVE}" -eq 1 && ! -e "${OUTPUT_DIR}" && -e "${WORK_ROOT}/previous" ]]; then
    mv "${WORK_ROOT}/previous" "${OUTPUT_DIR}" || true
  fi
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
  exit "${rc}"
}
trap cleanup EXIT

WORK_ROOT="$(mktemp -d "$(dirname "${OUTPUT_DIR}")/.package-runtime.XXXXXX")"
STAGE_DIR="${WORK_ROOT}/stage"
mkdir -p "${STAGE_DIR}/bin" "${STAGE_DIR}/lib"

cp "${BUILD_DIR}/cam_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/imu_reader_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/sensor_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/mosaic_rtsp_demo" "${STAGE_DIR}/bin/"
cp "${BUILD_DIR}/serial_port_demo" "${STAGE_DIR}/bin/"
for library in \
  libicm42688.so.2.0.0 libicm42688.so.2 libicm42688.so \
  libsc132.so.2.0.0 libsc132.so.2 libsc132.so \
  libprrtsp.so.2.0.0 libprrtsp.so.2 libprrtsp.so; do
  cp "${PACKAGE_LIB_DIR}/${library}" "${STAGE_DIR}/lib/${library}"
done
mkdir -p "${STAGE_DIR}/config"
cp "${PROJECT_DIR}/config/sensor_config.yaml" "${STAGE_DIR}/config/"
cp "${PROJECT_DIR}/VERSION" "${STAGE_DIR}/VERSION"

for executable in cam_demo imu_reader_demo mosaic_rtsp_demo sensor_demo serial_port_demo; do
  "${STRIP_TOOL}" --strip-unneeded "${STAGE_DIR}/bin/${executable}"
done

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

for name in cam_demo imu_reader_demo mosaic_rtsp_demo sensor_demo serial_port_demo; do
  cat > "${STAGE_DIR}/${name}" <<EOF
#!/bin/sh
set -eu
DEMO_DIR="\$(cd "\$(dirname "\$0")" && pwd)"
export DEMO_DIR
. "\${DEMO_DIR}/env.sh"
exec "\${DEMO_DIR}/bin/${name}" "\$@"
EOF
done

chmod 755 "${STAGE_DIR}" "${STAGE_DIR}/bin" "${STAGE_DIR}/lib" "${STAGE_DIR}/config"
chmod 755 "${STAGE_DIR}/cam_demo" "${STAGE_DIR}/imu_reader_demo" "${STAGE_DIR}/mosaic_rtsp_demo" "${STAGE_DIR}/sensor_demo" "${STAGE_DIR}/serial_port_demo"
chmod 755 "${STAGE_DIR}/bin/cam_demo" "${STAGE_DIR}/bin/imu_reader_demo" "${STAGE_DIR}/bin/mosaic_rtsp_demo" "${STAGE_DIR}/bin/sensor_demo" "${STAGE_DIR}/bin/serial_port_demo"
chmod 644 "${STAGE_DIR}/VERSION" "${STAGE_DIR}/env.sh" "${STAGE_DIR}/config/sensor_config.yaml" "${STAGE_DIR}/lib/"*.so

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
