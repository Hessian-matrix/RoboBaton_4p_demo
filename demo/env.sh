#!/bin/sh
DEMO_DIR="${DEMO_DIR:-$(pwd)}"
DEMO_LD_LIBRARY_PATH="${DEMO_DIR}/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}:${LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="${DEMO_LD_LIBRARY_PATH}"
fi
unset DEMO_LD_LIBRARY_PATH
