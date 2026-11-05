#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'USAGE'
Usage: build_uvc.sh SDK_ROOT OUTPUT [options]
  --sc132-include DIR   Directory containing current ABI3 sc132camera.h (required)
  --producer-lib DIR    Exact installed producer package lib directory
                       (default SDK_ROOT/out/deploy/system/root/demo/lib)
  --sdk-lib-dir DIR     Matching X5 multimedia/hbmem runtime library directory
  --cxx PATH           AArch64 cross g++ (or set CXX / CROSS_COMPILE)
  --sysroot DIR        Optional compiler/link sysroot; default compiler's own

SDK_ROOT is the platform_source_code tree (hbre/libmm/inc must exist).
Only a real producer libsc132.so.3 and the SDK hardware JPEG encoder are linked;
this script never rebuilds/copies producer libraries or installs runtime files.
Runtime search paths: /root/demo/lib, /usr/hobot/lib. The image installer must
preserve libsc132's complete matching package dependencies and the SDK hbre
partition libraries, including their SONAME links. No libguvc dependency.
USAGE
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if [[ $# -lt 2 ]]; then usage >&2; exit 2; fi
SDK_ROOT=$1
OUTPUT=$2
shift 2
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SC132_INCLUDE=
PRODUCER_LIB=
SDK_LIB_DIR=
CXX_BIN=${CXX:-${CROSS_COMPILE:-/opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}g++}
SYSROOT=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sc132-include|--producer-lib|--sdk-lib-dir|--cxx|--sysroot)
      if [[ $# -lt 2 || -z $2 ]]; then printf 'Missing value for %s\n' "$1" >&2; exit 2; fi
      case "$1" in
        --sc132-include) SC132_INCLUDE=$2 ;;
        --producer-lib) PRODUCER_LIB=$2 ;;
        --sdk-lib-dir) SDK_LIB_DIR=$2 ;;
        --cxx) CXX_BIN=$2 ;;
        --sysroot) SYSROOT=$2 ;;
      esac
      shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done
SDK_ROOT=$(realpath -e -- "$SDK_ROOT")
if [[ -z $SC132_INCLUDE || ! -f $SC132_INCLUDE/sc132camera.h ]]; then
  echo '--sc132-include must identify the current producer ABI3 header directory' >&2; exit 1
fi
SC132_INCLUDE=$(realpath -e -- "$SC132_INCLUDE")
PRODUCER_LIB=$(realpath -e -- "${PRODUCER_LIB:-$SDK_ROOT/out/deploy/system/root/demo/lib}")
MEDIA_INCLUDE=$SDK_ROOT/hbre/libmm/inc
if [[ ! -f $MEDIA_INCLUDE/hb_media_codec.h || ! -f $PRODUCER_LIB/libsc132.so.3 ]]; then
  echo 'Missing SDK codec headers or exact installed libsc132.so.3' >&2; exit 1
fi
if [[ -z $SDK_LIB_DIR ]]; then
  for candidate in "$SDK_ROOT/out/deploy/hbre/lib" \
      "$SDK_ROOT/out/deploy/system/usr/hobot/lib" "$SDK_ROOT/out/deploy/system/usr/lib"; do
    if [[ -f $candidate/libmultimedia.so.1 && -f $candidate/libhbmem.so.1 ]]; then
      SDK_LIB_DIR=$candidate
      break
    fi
  done
fi
if [[ -z $SDK_LIB_DIR || ! -f $SDK_LIB_DIR/libmultimedia.so.1 || ! -f $SDK_LIB_DIR/libhbmem.so.1 ]]; then
  echo '--sdk-lib-dir must contain matching libmultimedia.so.1 and libhbmem.so.1 (hbre partition)' >&2; exit 1
fi
SDK_LIB_DIR=$(realpath -e -- "$SDK_LIB_DIR")
CXX_BIN=$(command -v -- "$CXX_BIN")
if [[ ! -x $CXX_BIN ]]; then echo 'AArch64 C++ compiler is not executable' >&2; exit 1; fi
case "$("$CXX_BIN" -dumpmachine)" in
  aarch64*-linux-gnu*) ;;
  *) echo 'Compiler must target AArch64 Linux GNU' >&2; exit 1 ;;
esac
STRIP_BIN=${CXX_BIN%g++}strip
if [[ ! -x $STRIP_BIN ]]; then echo 'Matching compiler-prefix strip is not executable' >&2; exit 1; fi
FLAGS=()
if [[ -n $SYSROOT ]]; then SYSROOT=$(realpath -e -- "$SYSROOT"); FLAGS+=("--sysroot=$SYSROOT"); fi
mkdir -p -- "$(dirname -- "$OUTPUT")"
OUTPUT=$(realpath -m -- "$OUTPUT")
if [[ $OUTPUT == "$SCRIPT_DIR/uvc_mosaic.cpp" || $OUTPUT == "$SCRIPT_DIR/build_uvc.sh" ]]; then
  echo 'Refusing to overwrite backend sources' >&2; exit 1
fi
TEMP_OUTPUT=$(mktemp -- "${OUTPUT}.XXXXXX")
trap 'rm -f -- "$TEMP_OUTPUT"' EXIT
"$CXX_BIN" "${FLAGS[@]}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread \
  -I"$SC132_INCLUDE" -I"$MEDIA_INCLUDE" "$SCRIPT_DIR/uvc_mosaic.cpp" \
  -L"$PRODUCER_LIB" -L"$SDK_LIB_DIR" \
  -Wl,-rpath-link,"$PRODUCER_LIB" -Wl,-rpath-link,"$SDK_LIB_DIR" \
  -Wl,-rpath-link,"$SDK_ROOT/out/deploy/system/usr/lib" \
  -Wl,-rpath-link,"$SDK_ROOT/out/deploy/system/lib" \
  -Wl,--disable-new-dtags,-rpath,/root/demo/lib:/usr/hobot/lib \
  -Wl,-z,relro,-z,now -Wl,--build-id=sha1 \
  -Wl,--no-as-needed -l:libsc132.so.3 -l:libmultimedia.so.1 -l:libhbmem.so.1 \
  -Wl,--as-needed -ldl -lrt -lm -static-libstdc++ -static-libgcc \
  -o "$TEMP_OUTPUT"
"$STRIP_BIN" --strip-all "$TEMP_OUTPUT"
chmod 0755 -- "$TEMP_OUTPUT"
mv -f -- "$TEMP_OUTPUT" "$OUTPUT"
trap - EXIT
printf 'Built x5-uvc-mosaic: %s\nProducer: %s\nSDK libraries: %s\n' "$OUTPUT" "$PRODUCER_LIB/libsc132.so.3" "$SDK_LIB_DIR"
