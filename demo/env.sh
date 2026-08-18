#!/bin/sh
DEMO_DIR="${DEMO_DIR:-$(pwd)}"
DEMO_LD_LIBRARY_PATH="${DEMO_DIR}/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}:${LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}"
fi
DEMO_PATH_PREFIX="${DEMO_DIR}/bin:${DEMO_DIR}"
if [ -n "${PATH:-}" ]; then
  export PATH="${DEMO_PATH_PREFIX}:${PATH}"
else
  export PATH="${DEMO_PATH_PREFIX}"
fi
unset DEMO_LD_LIBRARY_PATH DEMO_PATH_PREFIX
