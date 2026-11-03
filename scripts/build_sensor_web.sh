#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${PROJECT_DIR}/build_x5/sensor_web"
GO_EXECUTABLE="${SENSOR_WEB_GO_EXECUTABLE:-${GO:-go}}"

usage() {
  cat <<'USAGE'
Usage: scripts/build_sensor_web.sh [--output <path>] [--go <executable>]

Build the embedded sensor_web service for Linux ARM64 using exactly Go 1.27.3.
Set SENSOR_WEB_GO_EXECUTABLE or GO to an installed compiler. GOTOOLCHAIN=local
prevents automatic compiler downloads. Dependencies must already be provisioned
in GOMODCACHE; this build is offline, readonly, and never installs board tools.
The output retains Go ELF symbols for package identity verification.
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output|--go)
      if [[ $# -lt 2 || -z "$2" ]]; then
        echo "Missing value for $1" >&2
        exit 2
      fi
      if [[ "$1" == --output ]]; then OUTPUT="$2"; else GO_EXECUTABLE="$2"; fi
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
if ! GO_EXECUTABLE="$(command -v "${GO_EXECUTABLE}")" || [[ ! -x "${GO_EXECUTABLE}" ]]; then
  echo "sensor_web requires Go 1.27.3; set SENSOR_WEB_GO_EXECUTABLE or GO to its executable (automatic toolchain switching is disabled)." >&2
  exit 1
fi
export GOTOOLCHAIN=local GOENV=off GOFLAGS= GOWORK=off
export GOOS=linux GOARCH=arm64 GOARM64=v8.0 CGO_ENABLED=0 GOEXPERIMENT=
export GOPROXY=off GOSUMDB=off GOTELEMETRY=off
if [[ "$("${GO_EXECUTABLE}" env GOVERSION)" != go1.27.3 ]]; then
  echo "sensor_web requires exactly Go 1.27.3: ${GO_EXECUTABLE}" >&2
  exit 1
fi
VERSION="$(<"${PROJECT_DIR}/VERSION")"
if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "VERSION must contain a SemVer core version" >&2
  exit 1
fi
SOURCE_DIGEST="$(python3 "${SCRIPT_DIR}/verify_runtime_package.py" --web-source-digest)"
mkdir -p "$(dirname "${OUTPUT}")"
OUTPUT="$(cd "$(dirname "${OUTPUT}")" && pwd)/$(basename "${OUTPUT}")"
case "${OUTPUT}" in
  "${PROJECT_DIR}/web/"*|"${PROJECT_DIR}/src/"*|"${PROJECT_DIR}/scripts/"*)
    echo "Refusing sensor_web output inside source inputs: ${OUTPUT}" >&2
    exit 1
    ;;
esac
TEMP_OUTPUT="$(mktemp "${OUTPUT}.XXXXXX")"
trap 'rm -f "${TEMP_OUTPUT}"' EXIT
cd "${PROJECT_DIR}/web/server"
"${GO_EXECUTABLE}" build -mod=readonly -trimpath -buildvcs=false -buildmode=exe \
  -ldflags="-X main.version=${VERSION} -X main.sourceDigest=${SOURCE_DIGEST}" \
  -o "${TEMP_OUTPUT}" .
python3 "${SCRIPT_DIR}/verify_runtime_package.py" --sensor-web "${TEMP_OUTPUT}"
chmod 755 "${TEMP_OUTPUT}"
mv "${TEMP_OUTPUT}" "${OUTPUT}"
trap - EXIT
printf 'Built sensor_web: %s\n' "${OUTPUT}"
