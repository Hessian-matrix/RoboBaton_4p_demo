#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build_x5}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-/root/x5/cross_compile/new/toolchain/aarch64_x5_host_toolchain.cmake}"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
cmake --build "${BUILD_DIR}" --target serial_port_demo -j
