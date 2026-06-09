#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST=""
USER="root"
PASSWORD="${X5_PASS:-}"
REMOTE_DIR="/root/demo"
RUN_SECONDS=25
STARTUP_TIMEOUT=14
CHANNELS=4
WIDTH=1088
HEIGHT=1280
FPS=60
BPS=2000
ROTATE=90
URL_PATH="/PRR"
DIAG_INTERVAL_MS=1000
MIN_FPS=55
MIN_GOOD_FPS_SAMPLES=3
MAX_PIPELINE_DELAY_MS=80
MAX_SEND_MAX_MS=120
MAX_GROUP_SKEW_NS=1000000
MAX_RTSP_PTS_SKEW_MS=1
MAX_FRAME_ID_OFFSET_JITTER=0
MIN_GROUP_ID=""
OUTPUT_DIR="${PROJECT_DIR}/regression_logs"
KILL_EXISTING=0
KEEP_REMOTE_LOG=0
TRIGGER_MODE="software_gpio"

usage() {
  cat <<'USAGE'
Usage:
  scripts/cam_demo_regression.sh --host <x5-ip> [options]

Connection:
  --host <ip>                 X5 board IP, required
  --user <name>               SSH user, default root
  --password <password>       SSH password; or set X5_PASS. If omitted, SSH key auth is used
  --remote-dir <path>         Deployed demo directory, default /root/demo
  --kill-existing             Kill existing cam_demo before running

Run:
  --run-seconds <sec>         cam_demo runtime, default 25
  --startup-timeout <sec>     RTSP port wait timeout, default 14
  --fps <30|60>               Camera/encoder FPS, default 60
  --trigger-mode <mode>       SC132_TRIGGER_MODE: software_gpio, vin_lpwm, none; default software_gpio/GPIO417
  --output-dir <path>         Local log output directory, default ./regression_logs

Evaluation thresholds:
  --min-fps <value>           Minimum acceptable per-channel FPS, default 55
  --min-good-fps-samples <n>  Minimum FPS samples per channel above --min-fps, default 3
  --max-pipeline-delay-ms <n> Maximum pipeline_delay_ms, default 80
  --max-send-max-ms <n>       Maximum send_max_ms, default 120
  --max-group-skew-ns <n>     Maximum group_skew_ns, default 1000000
  --max-rtsp-pts-skew-ms <n>  Maximum RTSP PTS skew across channels, default 1
  --max-frame-id-offset-jitter <n>
                              Maximum jitter of per-group frame_id offsets, default 0
  --min-group-id <n>          Minimum observed group_id; default derived from run time and FPS

Other:
  --keep-remote-log           Keep /tmp regression log on the board
  -h, --help                  Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --user) USER="$2"; shift 2 ;;
    --password) PASSWORD="$2"; shift 2 ;;
    --remote-dir) REMOTE_DIR="$2"; shift 2 ;;
    --run-seconds) RUN_SECONDS="$2"; shift 2 ;;
    --startup-timeout) STARTUP_TIMEOUT="$2"; shift 2 ;;
    --fps) FPS="$2"; shift 2 ;;
    --rotate) ROTATE="$2"; shift 2 ;;
    --trigger-mode) TRIGGER_MODE="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --min-fps) MIN_FPS="$2"; shift 2 ;;
    --min-good-fps-samples) MIN_GOOD_FPS_SAMPLES="$2"; shift 2 ;;
    --max-pipeline-delay-ms) MAX_PIPELINE_DELAY_MS="$2"; shift 2 ;;
    --max-send-max-ms) MAX_SEND_MAX_MS="$2"; shift 2 ;;
    --max-group-skew-ns) MAX_GROUP_SKEW_NS="$2"; shift 2 ;;
    --max-rtsp-pts-skew-ms) MAX_RTSP_PTS_SKEW_MS="$2"; shift 2 ;;
    --max-frame-id-offset-jitter) MAX_FRAME_ID_OFFSET_JITTER="$2"; shift 2 ;;
    --min-group-id) MIN_GROUP_ID="$2"; shift 2 ;;
    --kill-existing) KILL_EXISTING=1; shift ;;
    --keep-remote-log) KEEP_REMOTE_LOG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "${HOST}" ]]; then
  echo "Missing --host" >&2
  usage >&2
  exit 2
fi

if [[ -n "${PASSWORD}" ]] && ! command -v sshpass >/dev/null 2>&1; then
  echo "sshpass is required when --password or X5_PASS is used" >&2
  exit 2
fi

if [[ -z "${MIN_GROUP_ID}" ]]; then
  if (( RUN_SECONDS > 8 )); then
    MIN_GROUP_ID=$(( (RUN_SECONDS - 8) * FPS / 2 ))
  else
    MIN_GROUP_ID=$(( FPS / 2 ))
  fi
fi

mkdir -p "${OUTPUT_DIR}"
RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
LOCAL_LOG="${OUTPUT_DIR}/cam_demo_regression_${HOST}_${RUN_ID}.log"
REMOTE_LOG="/tmp/cam_demo_regression_${RUN_ID}.log"
REMOTE_RC="/tmp/cam_demo_regression_${RUN_ID}.rc"
REMOTE_PID="/tmp/cam_demo_regression_${RUN_ID}.pid"
FAILURES=0
WARNINGS=0

ssh_remote() {
  if [[ -n "${PASSWORD}" ]]; then
    sshpass -p "${PASSWORD}" ssh \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null \
      -o ConnectTimeout=8 \
      "${USER}@${HOST}" "$@"
  else
    ssh \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null \
      -o ConnectTimeout=8 \
      "${USER}@${HOST}" "$@"
  fi
}

record_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}

record_warn() {
  echo "WARN: $*" >&2
  WARNINGS=$((WARNINGS + 1))
}

record_pass() {
  echo "PASS: $*"
}

check_tcp_port() {
  local port="$1"

  # 优先使用 nc；缺失时退回 bash /dev/tcp，便于开发机最小环境跑回归。
  if command -v nc >/dev/null 2>&1; then
    nc -z -w 2 "${HOST}" "${port}" >/dev/null 2>&1
  else
    timeout 2 bash -c "cat < /dev/null > /dev/tcp/${HOST}/${port}" >/dev/null 2>&1
  fi
}

max_numeric_field() {
  local field="$1"
  (valid_metric_lines | grep -oE "${field}=[0-9]+([.][0-9]+)?" || true) \
    | awk -F= 'BEGIN { max = 0 } { if (($2 + 0) > max) max = $2 + 0 } END { print max }'
}

frame_set_metric_lines() {
  # frameset 行由单个回调线程一次性打印，可直接代表同一 frame-set 的四路时间戳。
  awk '
    /^frameset group_id=[0-9]+ group_ts_ns=[0-9]+ group_skew_ns=[0-9]+ calc_skew_ns=[0-9]+ cam0\(seq=[0-9]+,frame_id=[0-9]+,camera_ts_ns=[0-9]+\) cam1\(seq=[0-9]+,frame_id=[0-9]+,camera_ts_ns=[0-9]+\) cam2\(seq=[0-9]+,frame_id=[0-9]+,camera_ts_ns=[0-9]+\) cam3\(seq=[0-9]+,frame_id=[0-9]+,camera_ts_ns=[0-9]+\)$/ {
      print
    }
  ' "${LOCAL_LOG}"
}

frame_set_max_numeric_field() {
  local field="$1"
  (frame_set_metric_lines | grep -oE "${field}=[0-9]+([.][0-9]+)?" || true) \
    | awk -F= 'BEGIN { max = 0 } { if (($2 + 0) > max) max = $2 + 0 } END { print max }'
}

valid_metric_lines() {
  # cam_demo 多线程 stdout 可能字符级交错，只采纳字段完整的诊断行参与阈值判定。
  awk '
    /^cam[0-3] fps=[0-9]+([.][0-9]+)? last_seq=[0-9]+ group_id=[0-9]+ group_skew_ns=[0-9]+ queue=[0-9]+\/[0-9]+ full_waits=[0-9]+ pipeline_delay_ms=[0-9]+ camera_ts_ns=[0-9]+( rtsp_ts_ns=[0-9]+)? send_avg_ms=[0-9]+([.][0-9]+)? send_max_ms=[0-9]+([.][0-9]+)? rtsp_endpoint=ch[1-4] rtsp_port=55[4-7]$/ {
      print
    }
  ' "${LOCAL_LOG}"
}

float_gt() {
  awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'
}

float_lt() {
  awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'
}

cleanup_remote() {
  # 远端测试使用 setsid 独立进程组，清理时先按进程组发信号再兜底按 PID 发信号。
  ssh_remote "if [ -f '${REMOTE_PID}' ]; then pid=\$(cat '${REMOTE_PID}' 2>/dev/null || true); if [ -n \"\${pid}\" ]; then kill -INT -\"\${pid}\" 2>/dev/null || kill -INT \"\${pid}\" 2>/dev/null || true; fi; fi; rm -f '${REMOTE_PID}' '${REMOTE_RC}' $([[ "${KEEP_REMOTE_LOG}" == "1" ]] && echo "" || echo "'${REMOTE_LOG}'")" >/dev/null 2>&1 || true
}

trap cleanup_remote EXIT

echo "== X5 cam_demo regression =="
echo "host=${HOST} remote_dir=${REMOTE_DIR} run_seconds=${RUN_SECONDS} fps=${FPS} trigger_mode=${TRIGGER_MODE}"

existing_processes="$(ssh_remote "pgrep -a 'cam_demo' 2>/dev/null || true")"
if [[ -n "${existing_processes}" ]]; then
  if [[ "${KILL_EXISTING}" == "1" ]]; then
    echo "${existing_processes}" >&2
    record_warn "existing camera process found; killing because --kill-existing is set"
    ssh_remote "pkill -INT cam_demo 2>/dev/null || true; sleep 2"
  else
    echo "${existing_processes}" >&2
    record_fail "existing cam_demo is running; rerun with --kill-existing or stop it manually"
    exit 1
  fi
fi

ssh_remote "cd '${REMOTE_DIR}' && test -x ./cam_demo && test -f lib/libsc132.so && test -f lib/libprrtsp.so"
record_pass "remote files exist"

ldd_output="$(ssh_remote "cd '${REMOTE_DIR}' && cam_bin=./cam_demo; [ -x ./bin/cam_demo ] && cam_bin=./bin/cam_demo; LD_LIBRARY_PATH=\$PWD/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib ldd \${cam_bin} | egrep 'libsc132|libprrtsp|libstdc' || true")"
echo "${ldd_output}"
if grep -qE "libsc132\.so => ${REMOTE_DIR}/.*/lib/libsc132\.so|libsc132\.so => ${REMOTE_DIR}/lib/libsc132\.so" <<<"${ldd_output}"; then
  record_pass "cam_demo loads local libsc132.so"
else
  record_fail "cam_demo does not load ${REMOTE_DIR}/lib/libsc132.so"
fi
if grep -qE "libprrtsp\.so => ${REMOTE_DIR}/.*/lib/libprrtsp\.so|libprrtsp\.so => ${REMOTE_DIR}/lib/libprrtsp\.so" <<<"${ldd_output}"; then
  record_pass "cam_demo loads local libprrtsp.so"
else
  record_fail "cam_demo does not load ${REMOTE_DIR}/lib/libprrtsp.so"
fi

# 后台 cam_demo 必须关闭 stdin 并脱离 SSH 会话，否则 ssh 会等到 timeout 结束才返回，导致端口探测发生在进程退出之后。
remote_env_cmd="export LD_LIBRARY_PATH='${REMOTE_DIR}/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib'"
# 远端进程显式导出触发模式，确保本次回归验证的是指定同步触发路径。
remote_env_cmd="${remote_env_cmd} && export SC132_TRIGGER_MODE='${TRIGGER_MODE}'"
remote_run_cmd="cd '${REMOTE_DIR}' && ${remote_env_cmd} && timeout '${RUN_SECONDS}' ./cam_demo --channels '${CHANNELS}' --fps '${FPS}' --width '${WIDTH}' --height '${HEIGHT}' --bps '${BPS}' --rotate '${ROTATE}' --url '${URL_PATH}' --diagnostics --diag-interval-ms '${DIAG_INTERVAL_MS}' > '${REMOTE_LOG}' 2>&1; rc=\\\$?; echo \\\${rc} > '${REMOTE_RC}'"
ssh_remote "rm -f '${REMOTE_LOG}' '${REMOTE_RC}' '${REMOTE_PID}'; nohup setsid sh -c \"${remote_run_cmd}\" </dev/null >/dev/null 2>&1 & echo \$! > '${REMOTE_PID}'"
run_start_seconds=${SECONDS}

declare -A PORT_OK=()
for port in 554 555 556 557; do
  PORT_OK["${port}"]=0
done

deadline=$((SECONDS + STARTUP_TIMEOUT))
while (( SECONDS < deadline )); do
  all_open=1
  for port in 554 555 556 557; do
    if [[ "${PORT_OK[${port}]}" == "0" ]] && check_tcp_port "${port}"; then
      PORT_OK["${port}"]=1
      echo "PASS: RTSP port ${port} opened"
    fi
    if [[ "${PORT_OK[${port}]}" == "0" ]]; then
      all_open=0
    fi
  done
  (( all_open == 1 )) && break
  sleep 1
done

for port in 554 555 556 557; do
  if [[ "${PORT_OK[${port}]}" == "0" ]]; then
    record_fail "RTSP port ${port} did not open within ${STARTUP_TIMEOUT}s"
  fi
done

remote_rc=""
# 端口探测会消耗启动窗口，等待 rc 必须按远端启动时刻计算，避免正常 timeout 收尾被误判为未结束。
wait_deadline=$((run_start_seconds + RUN_SECONDS + STARTUP_TIMEOUT + 8))
while (( SECONDS < wait_deadline )); do
  remote_rc="$(ssh_remote "cat '${REMOTE_RC}' 2>/dev/null || true")"
  [[ -n "${remote_rc}" ]] && break
  sleep 1
done

if [[ -z "${remote_rc}" ]]; then
  record_fail "cam_demo did not finish within expected time"
  ssh_remote "if [ -f '${REMOTE_PID}' ]; then kill -INT \$(cat '${REMOTE_PID}') 2>/dev/null || true; fi"
else
  echo "remote timeout wrapper rc=${remote_rc}"
  if [[ "${remote_rc}" == "124" || "${remote_rc}" == "0" ]]; then
    record_pass "cam_demo runtime completed under timeout wrapper"
  else
    record_fail "cam_demo exited with unexpected rc=${remote_rc}"
  fi
fi

ssh_remote "cat '${REMOTE_LOG}' 2>/dev/null || true" > "${LOCAL_LOG}"
echo "log=${LOCAL_LOG}"

fatal_pattern='Segmentation fault|core dumped|symbol lookup error|GLIBCXX|undefined symbol|No Camera Sensor found|create_and_run_vflow failed|failed for sensor|ret[ =-]+-36|ret[ =-]+-10|init RTSP channel .* failed|invalid NV12 DMA|transform pool exhausted|queue_full=[1-9][0-9]*'
fatal_matches="$(grep -Ein "${fatal_pattern}" "${LOCAL_LOG}" || true)"
if [[ -n "${fatal_matches}" ]]; then
  echo "${fatal_matches}" >&2
  record_fail "fatal/error patterns found in log"
else
  record_pass "no fatal/error patterns found"
fi

sensor_count="$(grep -c 'INFO: Found sensor_name:sc132gs-1280p' "${LOCAL_LOG}" || true)"
if (( sensor_count >= CHANNELS )); then
  record_pass "sensor detection count=${sensor_count}"
else
  record_fail "sensor detection count=${sensor_count}, expected >=${CHANNELS}"
fi

encoder_count="$(grep -Ec 'Encode idx: [0-3], init successful' "${LOCAL_LOG}" || true)"
if (( encoder_count >= CHANNELS )); then
  record_pass "RTSP encoder init count=${encoder_count}"
else
  record_fail "RTSP encoder init count=${encoder_count}, expected >=${CHANNELS}"
fi

set +e
fps_report="$(
  valid_metric_lines \
    | awk -v channels="${CHANNELS}" -v min_fps="${MIN_FPS}" -v min_samples="${MIN_GOOD_FPS_SAMPLES}" '
      {
        cam = substr($1, 4, 1)
        split($2, pair, "=")
        fps = pair[2] + 0
        total[cam]++
        last[cam] = fps
        if (fps >= min_fps) {
          good[cam]++
        }
      }
      END {
        failed = 0
        for (i = 0; i < channels; i++) {
          printf("cam%d fps_last=%.2f good_samples=%d total_samples=%d\n", i, last[i] + 0, good[i] + 0, total[i] + 0)
          if ((good[i] + 0) < min_samples || (last[i] + 0) < min_fps) {
            failed = 1
          }
        }
        exit failed
      }'
)"
fps_status=$?
set -e
echo "${fps_report}"
if (( fps_status == 0 )); then
  record_pass "per-channel FPS meets threshold >=${MIN_FPS}"
else
  record_fail "per-channel FPS does not meet threshold >=${MIN_FPS}"
fi

max_group_id="$( (valid_metric_lines | grep -oE 'group_id=[0-9]+' || true) \
  | awk -F= 'BEGIN { max = 0 } { if (($2 + 0) > max) max = $2 + 0 } END { print max }')"
if (( max_group_id >= MIN_GROUP_ID )); then
  record_pass "group_id progressed to ${max_group_id} >= ${MIN_GROUP_ID}"
else
  record_fail "group_id progressed only to ${max_group_id}, expected >= ${MIN_GROUP_ID}"
fi

# 队列、延迟、skew 等门限只从完整诊断行取值，避免多线程日志交错造成误判。
if valid_metric_lines | grep -qE 'full_waits=[1-9][0-9]*'; then
  record_fail "queue full_waits occurred"
else
  record_pass "full_waits remained zero"
fi

max_pipeline_delay_ms="$(max_numeric_field 'pipeline_delay_ms')"
if float_gt "${max_pipeline_delay_ms}" "${MAX_PIPELINE_DELAY_MS}"; then
  record_fail "max pipeline_delay_ms=${max_pipeline_delay_ms} > ${MAX_PIPELINE_DELAY_MS}"
else
  record_pass "max pipeline_delay_ms=${max_pipeline_delay_ms}"
fi

max_send_max_ms="$(max_numeric_field 'send_max_ms')"
if float_gt "${max_send_max_ms}" "${MAX_SEND_MAX_MS}"; then
  record_fail "max send_max_ms=${max_send_max_ms} > ${MAX_SEND_MAX_MS}"
else
  record_pass "max send_max_ms=${max_send_max_ms}"
fi

max_group_skew_ns="$(max_numeric_field 'group_skew_ns')"
frame_set_line_count="$(frame_set_metric_lines | wc -l)"
frame_set_max_group_skew_ns="$(frame_set_max_numeric_field 'group_skew_ns')"
if (( frame_set_line_count > 0 )); then
  max_group_skew_ns="${frame_set_max_group_skew_ns}"
fi
if float_gt "${max_group_skew_ns}" "${MAX_GROUP_SKEW_NS}"; then
  record_fail "max group_skew_ns=${max_group_skew_ns} > ${MAX_GROUP_SKEW_NS}"
else
  record_pass "max group_skew_ns=${max_group_skew_ns}"
fi

set +e
frame_id_offset_report="$(
  frame_set_metric_lines \
    | awk -v max_jitter="${MAX_FRAME_ID_OFFSET_JITTER}" '
      {
        n = split($0, parts, /frame_id=/)
        if (n < 5) {
          next
        }
        for (i = 1; i <= 4; i++) {
          split(parts[i + 1], tail, ",")
          fid[i - 1] = tail[1] + 0
        }
        off1 = fid[1] - fid[0]
        off2 = fid[2] - fid[0]
        off3 = fid[3] - fid[0]
        if (count == 0) {
          min1 = max1 = off1
          min2 = max2 = off2
          min3 = max3 = off3
        }
        if (off1 < min1) min1 = off1
        if (off1 > max1) max1 = off1
        if (off2 < min2) min2 = off2
        if (off2 > max2) max2 = off2
        if (off3 < min3) min3 = off3
        if (off3 > max3) max3 = off3
        count++
      }
      END {
        jitter1 = max1 - min1
        jitter2 = max2 - min2
        jitter3 = max3 - min3
        max_observed = jitter1
        if (jitter2 > max_observed) max_observed = jitter2
        if (jitter3 > max_observed) max_observed = jitter3
        printf("frameset_count=%d frame_id_offset cam1-cam0=[%d,%d] cam2-cam0=[%d,%d] cam3-cam0=[%d,%d] max_jitter=%d\n",
               count, min1, max1, min2, max2, min3, max3, max_observed)
        if (count == 0 || max_observed > max_jitter) {
          exit 1
        }
      }'
)"
frame_id_offset_status=$?
set -e
echo "${frame_id_offset_report}"
if (( frame_id_offset_status == 0 )); then
  record_pass "frame_id offsets stable with jitter <= ${MAX_FRAME_ID_OFFSET_JITTER}"
else
  record_fail "frame_id offsets jitter exceeds ${MAX_FRAME_ID_OFFSET_JITTER}"
fi

rtsp_group_report="$(
  valid_metric_lines \
    | awk -v channels="${CHANNELS}" -v max_skew_ms="${MAX_RTSP_PTS_SKEW_MS}" '
      {
        cam = substr($1, 4, 1) + 0
        group_id = ""
        rtsp_ts = ""
        for (i = 1; i <= NF; i++) {
          if ($i ~ /^group_id=/) {
            split($i, pair, "=")
            group_id = pair[2]
          } else if ($i ~ /^rtsp_ts_ns=/) {
            split($i, pair, "=")
            rtsp_ts = pair[2] + 0
          }
        }
        if (group_id == "" || rtsp_ts == "") {
          next
        }
        key = group_id
        mask[key] = or(mask[key], lshift(1, cam))
        if (!(key in min_ts) || rtsp_ts < min_ts[key]) {
          min_ts[key] = rtsp_ts
        }
        if (!(key in max_ts) || rtsp_ts > max_ts[key]) {
          max_ts[key] = rtsp_ts
        }
      }
      END {
        complete = 0
        bad = 0
        max_skew = 0
        expected_mask = lshift(1, channels) - 1
        for (key in mask) {
          if (mask[key] != expected_mask) {
            continue
          }
          complete++
          skew_ms = (max_ts[key] - min_ts[key]) / 1000000.0
          if (skew_ms > max_skew) {
            max_skew = skew_ms
          }
          if (skew_ms > max_skew_ms) {
            bad++
          }
        }
        printf("complete_rtsp_groups=%d bad_rtsp_groups=%d max_group_rtsp_pts_skew_ms=%.3f\n",
               complete, bad, max_skew)
        if (complete == 0 || bad > 0) {
          exit 1
        }
      }'
)"
rtsp_group_status=$?
echo "${rtsp_group_report}"
if (( rtsp_group_status == 0 )); then
  record_pass "per-group RTSP PTS skew <= ${MAX_RTSP_PTS_SKEW_MS}ms"
else
  record_fail "per-group RTSP PTS skew check failed"
fi

snapshot_rtsp_pts_skew_ms="$( (grep -oE 'rtsp_pts_skew_ms=[0-9]+([.][0-9]+)?' "${LOCAL_LOG}" || true) \
  | awk -F= 'BEGIN { max = 0 } { if (($2 + 0) > max) max = $2 + 0 } END { print max }')"
if float_gt "${snapshot_rtsp_pts_skew_ms}" "${MAX_RTSP_PTS_SKEW_MS}"; then
  record_warn "snapshot rtsp_pts_skew_ms=${snapshot_rtsp_pts_skew_ms}; latest-frame snapshots may span different groups"
else
  record_pass "snapshot rtsp_pts_skew_ms=${snapshot_rtsp_pts_skew_ms}"
fi

frame_index_drop_count="$(grep -c 'sample_reason=frame-index' "${LOCAL_LOG}" || true)"
base_skew_drop_count="$(grep -c 'sample_reason=base-skew' "${LOCAL_LOG}" || true)"
if (( frame_index_drop_count > 0 )); then
  record_warn "frame-index drop reports=${frame_index_drop_count}; allowed if FPS/group_id checks pass"
fi
if (( base_skew_drop_count > 0 )); then
  record_warn "base-skew cleanup reports=${base_skew_drop_count}; allowed during startup recovery"
fi

if grep -q 'SC132 4-camera RTSP demo stopped' "${LOCAL_LOG}"; then
  record_pass "cam_demo stopped cleanly"
else
  record_warn "clean stop line not found; check timeout signal handling"
fi

if (( FAILURES > 0 )); then
  echo "RESULT: FAIL failures=${FAILURES} warnings=${WARNINGS}"
  exit 1
fi

echo "RESULT: PASS warnings=${WARNINGS}"
