#!/bin/sh
set -eu

# sensor_demo 启动与开机自启动管理脚本。
#
# 无参数前台启动；stop/restart 管理本包进程；enable/disable 管理开机自启动。
#
# 开机自启动通过 /userdata/startup.sh 实现：板端 /etc/init.d/S99auto_startup
# 在 /userdata 挂载后执行该文件。本脚本只追加/删除自己带标记的块，
# 不会覆盖其它应用写入的内容；系统 wifi_setup 使用独立启动入口。
#
# 开机自启动使用运行包内 config/sensor_config.yaml 的默认配置。

AUTOSTART_FILE="${SENSOR_DEMO_AUTOSTART_FILE:-/userdata/startup.sh}"
AUTOSTART_BEGIN="# SENSOR_DEMO_AUTOSTART_MANAGED"
AUTOSTART_END="# SENSOR_DEMO_AUTOSTART_END"
BOOT_LOG="${SENSOR_DEMO_LOG_FILE:-/userdata/sensor_demo_autostart.log}"
CAM_SERVICE_WAIT_S="${CAM_SERVICE_WAIT_S:-15}"
STOP_TIMEOUT_S="${SENSOR_DEMO_STOP_TIMEOUT_S:-1800}"

SCRIPT_PATH="$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")"
DEMO_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
用法：$(basename "$0") [stop|restart|enable|disable|status|help]

  （无参数）  前台启动 sensor_demo
  stop        正常停止本运行包的 sensor_demo，等待录制与 Web 子进程收尾
  restart     正常停止后按包内配置后台启动，停止失败则不启动
  enable      安装 sensor_demo 开机自启动
  disable     取消 sensor_demo 开机自启动
  status      显示自启动与运行状态
  help        显示本帮助

如需带参数启动 sensor_demo，请直接运行 ./sensor_demo <参数>。
开机自启动使用包内默认配置，直接修改 config/sensor_config.yaml 即可生效。
stop/restart 不改变开机自启动设置，不停止 cam-service，不强杀进程。
停止最多等待 ${STOP_TIMEOUT_S} 秒，可用 SENSOR_DEMO_STOP_TIMEOUT_S 调整。
restart 日志：${BOOT_LOG}；返回成功仅表示启动后 2 秒内未退出。
EOF
}

run_foreground() {
    export DEMO_DIR
    . "${DEMO_DIR}/env.sh"
    exec "${DEMO_DIR}/bin/sensor_demo"
}

# pidfd 将信号和退出等待绑定到同一个进程，避免 PID 复用误伤其它应用。
manage_runtime() {
    python3 - "${DEMO_DIR}" "$1" "${BOOT_LOG}" "${STOP_TIMEOUT_S}" <<'PY'
import fcntl
import hashlib
import math
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

base = Path(sys.argv[1]).resolve()
action, log_path = sys.argv[2:4]

def package_processes(name):
    found = []
    try:
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                executable = os.readlink(entry / 'exe').removesuffix(' (deleted)')
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
            if executable != str(base / 'bin' / name):
                continue
            fd = None
            try:
                fd = os.pidfd_open(int(entry.name))
                if os.readlink(entry / 'exe').removesuffix(' (deleted)') == executable:
                    found.append((int(entry.name), fd))
                    fd = None
            except (FileNotFoundError, ProcessLookupError):
                pass
            finally:
                if fd is not None:
                    os.close(fd)
        return found
    except BaseException:
        for _, fd in found:
            os.close(fd)
        raise

def stop_runtime(timeout):
    mains = package_processes('sensor_demo')
    helpers = []
    try:
        helpers = package_processes('sensor_web')
        poller = select.poll()
        for _, fd in mains + helpers:
            poller.register(fd, select.POLLIN)
        for pid, fd in mains:
            try:
                signal.pidfd_send_signal(fd, signal.SIGINT)
                print(f'sensor_demo stopping (pid {pid})', flush=True)
            except ProcessLookupError:
                pass
        pending = {fd for _, fd in mains + helpers}
        deadline = time.monotonic() + timeout
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError('正常停止超时；保留进程，不强杀、不启动新实例')
            for fd, _ in poller.poll(min(1000, max(1, int(remaining * 1000)))):
                pending.discard(fd)
                poller.unregister(fd)
        print('sensor_demo=stopped', flush=True)
    finally:
        for _, fd in mains + helpers:
            os.close(fd)

def start_runtime(log):
    process = subprocess.Popen([str(base / 'bin/sensor_demo')], cwd=base,
                               stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                               start_new_session=True)
    try:
        code = process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        print(f'sensor_demo started (pid {process.pid}), log={log_path}', flush=True)
        return
    raise RuntimeError(f'sensor_demo 启动后退出，退出码 {code}；检查 {log_path}')

try:
    if action == 'status':
        processes = package_processes('sensor_demo')
        try:
            print('sensor_demo=' + ('running' if processes else 'stopped'))
        finally:
            for _, fd in processes:
                os.close(fd)
    else:
        timeout = float(sys.argv[4])
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('SENSOR_DEMO_STOP_TIMEOUT_S 必须为正数')
        key = hashlib.sha256(os.fsencode(base)).hexdigest()
        lock_fd = os.open(f'/tmp/sensor-demo-{key}.lock',
                          os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        with os.fdopen(lock_fd, 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if action == 'stop':
                stop_runtime(timeout)
            else:
                if not os.access(base / 'bin/sensor_demo', os.X_OK):
                    raise RuntimeError('缺少可执行文件：' + str(base / 'bin/sensor_demo'))
                with open(log_path, 'ab', buffering=0) as log:
                    if action == 'restart':
                        stop_runtime(timeout)
                    else:
                        existing = package_processes('sensor_demo')
                        try:
                            existing += package_processes('sensor_web')
                        finally:
                            for _, fd in existing:
                                os.close(fd)
                        if existing:
                            print('sensor_demo already running, skip autostart', flush=True)
                            sys.exit(0)
                    start_runtime(log)
except (OSError, ValueError, RuntimeError) as error:
    print(f'ERROR: {error}', file=sys.stderr)
    sys.exit(1)
PY
}

restart_runtime() {
    export DEMO_DIR
    . "${DEMO_DIR}/env.sh"
    manage_runtime restart
}

# 开机模式：等待 cam-service 完成相机初始化后，后台启动 sensor_demo，
# 输出写入 /userdata 日志，避免阻塞 S99auto_startup 的启动流程。
run_boot() {
    i=0
    while [ "${i}" -lt "${CAM_SERVICE_WAIT_S}" ]; do
        pgrep -x cam-service >/dev/null 2>&1 && break
        sleep 1
        i=$((i + 1))
    done
    # 给 cam-service 留出打开相机的初始化时间。
    sleep 1
    if pgrep -x sensor_demo >/dev/null 2>&1; then
        echo "sensor_demo already running, skip autostart" >>"${BOOT_LOG}"
        exit 0
    fi
    export DEMO_DIR
    . "${DEMO_DIR}/env.sh"
    manage_runtime start
}

enable_autostart() {
    [ "$(id -u)" = "0" ] || die "enable 需要 root 权限，请使用 root 执行"
    [ -x "${DEMO_DIR}/bin/sensor_demo" ] || die "缺少可执行文件：${DEMO_DIR}/bin/sensor_demo"
    [ -f /etc/init.d/S99auto_startup ] ||
        warn "未检测到 /etc/init.d/S99auto_startup，开机自启动可能不会生效"

    block="$(mktemp)"
    {
        printf '%s\n' "${AUTOSTART_BEGIN}"
        printf '# 由 %s enable 生成：开机自启动 sensor_demo。\n' "$(basename "${SCRIPT_PATH}")"
        printf '%s --boot >/dev/null 2>&1\n' "${SCRIPT_PATH}"
        printf '%s\n' "${AUTOSTART_END}"
    } >"${block}"

    tmp="$(mktemp)"
    if [ -f "${AUTOSTART_FILE}" ]; then
        # 先移除旧的 sensor_demo 块，保留文件内其它内容，实现多脚本共存。
        sed "/^${AUTOSTART_BEGIN}$/,/^${AUTOSTART_END}$/d" "${AUTOSTART_FILE}" >"${tmp}" ||
            die "无法处理 ${AUTOSTART_FILE}"
    else
        : >"${tmp}"
    fi

    if [ -s "${tmp}" ]; then
        { cat "${tmp}"; printf '\n'; cat "${block}"; } >"${AUTOSTART_FILE}.new"
    else
        { printf '#!/bin/sh\n\n'; cat "${block}"; } >"${AUTOSTART_FILE}.new"
    fi
    mv "${AUTOSTART_FILE}.new" "${AUTOSTART_FILE}"
    chmod 755 "${AUTOSTART_FILE}"
    sync
    rm -f "${tmp}" "${block}"
    info "已启用 sensor_demo 开机自启动：${AUTOSTART_FILE}"
}

disable_autostart() {
    [ "$(id -u)" = "0" ] || die "disable 需要 root 权限，请使用 root 执行"
    if [ ! -f "${AUTOSTART_FILE}" ]; then
        info "未启用开机自启动：${AUTOSTART_FILE} 不存在"
        return 0
    fi

    tmp="$(mktemp)"
    sed "/^${AUTOSTART_BEGIN}$/,/^${AUTOSTART_END}$/d" "${AUTOSTART_FILE}" >"${tmp}" ||
        die "无法处理 ${AUTOSTART_FILE}"
    # 只保留含实际命令的文件；仅剩 shebang、注释或空行的残留直接删除。
    if [ -s "${tmp}" ] && grep -qE '^[^#].*[^[:space:]]' "${tmp}"; then
        mv "${tmp}" "${AUTOSTART_FILE}"
        chmod 755 "${AUTOSTART_FILE}"
        info "已移除 sensor_demo 开机自启动块：${AUTOSTART_FILE}"
    else
        rm -f "${AUTOSTART_FILE}"
        info "已移除开机自启动文件：${AUTOSTART_FILE}"
    fi
    sync
}

status_autostart() {
    if [ -f "${AUTOSTART_FILE}" ] && grep -q "^${AUTOSTART_BEGIN}$" "${AUTOSTART_FILE}" 2>/dev/null; then
        info "autostart=enabled (${AUTOSTART_FILE})"
    else
        info "autostart=disabled"
    fi
    manage_runtime status
}

case "${1:-}" in
    "")
        run_foreground
        ;;
    stop)
        manage_runtime stop
        ;;
    restart)
        restart_runtime
        ;;
    enable)
        enable_autostart
        ;;
    disable)
        disable_autostart
        ;;
    status)
        status_autostart
        ;;
    --boot)
        run_boot
        ;;
    help|-h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
