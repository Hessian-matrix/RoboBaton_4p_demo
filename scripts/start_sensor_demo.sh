#!/bin/sh
set -eu

# sensor_demo 启动与开机自启动管理脚本。
#
# 无参数前台启动；stop/restart 管理本包进程；enable/disable 管理开机自启动。
#
# 开机入口 /etc/init.d/S98sensor_demo_autostart 读取系统分区策略。
# 缺失策略默认开启；普通重启保留设置，重烧 system 恢复镜像默认值。
# 旧 /userdata/startup.sh 的调用同样受策略约束，不读取旧注册状态。
#
# 开机自启动使用运行包内 config/sensor_config.yaml 的默认配置。

AUTOSTART_FILE="${SENSOR_DEMO_AUTOSTART_FILE:-/etc/4p/sensor-demo-autostart}"
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
开机自启动与 restart 使用包内配置，但关闭 IMU 样本打印；前台启动不覆盖打印配置。
stop/restart 不改变开机自启动设置，不停止 cam-service，不强杀进程。
停止最多等待 ${STOP_TIMEOUT_S} 秒，可用 SENSOR_DEMO_STOP_TIMEOUT_S 调整。
restart 日志：${BOOT_LOG}；成功需所选相机和 IMU 连续推进至少 5 秒。
启动健康检查默认最多 30 秒；可用 SENSOR_DEMO_START_TIMEOUT_S 调整。
后台日志保留启动、错误、录制结果与退出健康摘要，不影响 IMU 采集、网页或保存。
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
import socket
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

def start_runtime(log, stop_timeout):
    # Background logging excludes sample dumps, not acquisition or final health.
    timeout = float(os.environ.get('SENSOR_DEMO_START_TIMEOUT_S', '30'))
    stable_seconds = float(os.environ.get('SENSOR_DEMO_START_STABLE_S', '5'))
    if not math.isfinite(timeout) or not math.isfinite(stable_seconds) or not 0 < stable_seconds < timeout:
        raise ValueError('启动超时必须大于健康观察时长，且均为有限正数')
    reader, writer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    process = None
    try:
        process = subprocess.Popen([str(base / 'bin/sensor_demo'), '--print-rate-hz', '0'], cwd=base,
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            pass_fds=(writer.fileno(),),
            env=dict(os.environ, SENSOR_DEMO_STARTUP_FD=str(writer.fileno())))
        writer.close()
        reader.settimeout(.5)
        deadline = time.monotonic() + timeout
        previous = None
        previous_at = stable_at = None
        while time.monotonic() < deadline:
            code = process.poll()
            if code is not None:
                raise RuntimeError(f'sensor_demo 启动后退出，退出码 {code}；检查 {log_path}')
            try:
                data, _, flags, _ = reader.recvmsg(512)
            except socket.timeout:
                if previous_at is None or time.monotonic()-previous_at >= 1:
                    stable_at = None
                continue
            if not data:
                try:
                    code = process.wait(timeout=.1)
                except subprocess.TimeoutExpired:
                    code = None
                if code is not None:
                    raise RuntimeError(f'sensor_demo 启动后退出，退出码 {code}；检查 {log_path}')
                raise RuntimeError('sensor_demo 未完成数据健康检查即关闭启动通道')
            values = data.decode('ascii').split()
            if flags & socket.MSG_TRUNC or len(values) != 8 or any(not v.isdecimal() for v in values):
                raise RuntimeError('sensor_demo 启动健康报告格式无效')
            version, pid, mask, *counts = map(int, values)
            if version != 1 or pid != process.pid or not 0 < mask <= 15 or any(v > 2**64-1 for v in counts):
                raise RuntimeError('sensor_demo 启动健康报告身份无效')
            now = time.monotonic()
            selected = [counts[i] for i in range(4) if mask & (1 << i)] + [counts[4]]
            progressing = previous is not None and previous[0] == mask and now-previous_at < 1 and all(
                current > prior for current, prior in zip(selected, previous[1]))
            if not progressing or not all(selected):
                stable_at = None
            elif stable_at is None:
                stable_at = now
            previous, previous_at = (mask, selected), now
            if stable_at is not None and now-stable_at >= stable_seconds and process.poll() is None:
                print(f'sensor_demo started (pid {process.pid}), capture/IMU progressing, log={log_path}', flush=True)
                return
        raise RuntimeError(f'sensor_demo 在 {timeout:g} 秒内未达到持续采集健康条件；检查 {log_path}')
    except BaseException:
        if process is not None and process.poll() is None:
            # Only this launch's child is stopped; do not kill an unrelated
            # package or skip recording cleanup when startup health times out.
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=stop_timeout)
            except subprocess.TimeoutExpired:
                print('ERROR: 启动失败且正常收尾超时；保留进程，未强杀', file=sys.stderr)
        raise
    finally:
        reader.close()
        writer.close()

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
                    start_runtime(log, timeout)
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

# 开机模式等待 cam-service 初始化；只读策略，不自动重写用户选择。
run_boot() {
    read_autostart
    [ "$AUTOSTART_STATE" = enabled ] || return 0
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

read_autostart() {
    AUTOSTART_STATE=enabled
    parent="$(dirname "$AUTOSTART_FILE")"
    for directory in /etc "$parent"; do
        [ ! -L "$directory" ] || die "策略目录不能是符号链接：$directory"
        if [ -e "$directory" ]; then
            [ -d "$directory" ] || die "策略目录无效：$directory"
            case "$(stat -c '%u:%g:%a' "$directory")" in
                0:0:755|0:0:750|0:0:700|0:0:555|0:0:550|0:0:500) ;;
                *) die "策略目录权限不安全：$directory" ;;
            esac
        fi
    done
    if [ -e "$AUTOSTART_FILE" ] || [ -L "$AUTOSTART_FILE" ]; then
        [ -f "$AUTOSTART_FILE" ] && [ ! -L "$AUTOSTART_FILE" ] &&
            [ "$(stat -c '%u:%g:%a' "$AUTOSTART_FILE")" = 0:0:600 ] ||
            die "策略文件不可信：$AUTOSTART_FILE"
        if printf 'enabled\n' | cmp -s - "$AUTOSTART_FILE"; then
            AUTOSTART_STATE=enabled
        elif printf 'disabled\n' | cmp -s - "$AUTOSTART_FILE"; then
            AUTOSTART_STATE=disabled
        else
            die "策略文件无效：$AUTOSTART_FILE"
        fi
    fi
}

set_autostart() {
    [ "$(id -u)" = 0 ] || die "$1 需要 root 权限"
    read_autostart
    if [ ! -d "$parent" ]; then
        mkdir -m 755 "$parent" || die "无法创建系统策略目录：$parent"
    fi
    temporary="$(mktemp "$parent/.sensor-demo-autostart.XXXXXX")" ||
        die "无法写入系统策略目录：$parent"
    trap 'rm -f "$temporary"' EXIT
    trap 'exit 1' HUP INT TERM
    printf '%s\n' "$1" > "$temporary"
    chmod 600 "$temporary"
    mv -f "$temporary" "$AUTOSTART_FILE"
    sync
    info "autostart=$1 ($AUTOSTART_FILE)"
}

enable_autostart() {
    [ -x "${DEMO_DIR}/bin/sensor_demo" ] || die "缺少可执行文件：${DEMO_DIR}/bin/sensor_demo"
    [ -f /etc/init.d/S98sensor_demo_autostart ] ||
        warn "未检测到 /etc/init.d/S98sensor_demo_autostart，需安装配套系统启动入口"
    set_autostart enabled
}

disable_autostart() {
    set_autostart disabled
}

status_autostart() {
    read_autostart
    info "autostart=$AUTOSTART_STATE ($AUTOSTART_FILE)"
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
