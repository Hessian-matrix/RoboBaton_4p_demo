#!/bin/sh
set -eu

# sensor_demo 启动与开机自启动管理脚本。
#
# 无参数：前台启动 sensor_demo；enable/disable/status：管理开机自启动。
#
# 开机自启动通过 /userdata/startup.sh 实现：板端 /etc/init.d/S99auto_startup
# 在 /userdata 挂载后执行该文件。本脚本只追加/删除自己带标记的块，
# 不会覆盖其它脚本（如 wifi_setup.sh）写入的内容。
#
# 开机自启动使用运行包内 config/sensor_config.yaml 的默认配置。

AUTOSTART_FILE="${SENSOR_DEMO_AUTOSTART_FILE:-/userdata/startup.sh}"
AUTOSTART_BEGIN="# SENSOR_DEMO_AUTOSTART_MANAGED"
AUTOSTART_END="# SENSOR_DEMO_AUTOSTART_END"
BOOT_LOG="/userdata/sensor_demo_autostart.log"
CAM_SERVICE_WAIT_S="${CAM_SERVICE_WAIT_S:-15}"

SCRIPT_PATH="$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")"
DEMO_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
用法：$(basename "$0") [enable|disable|status|help]

  （无参数）  前台启动 sensor_demo
  enable      安装 sensor_demo 开机自启动
  disable     取消 sensor_demo 开机自启动
  status      显示自启动与运行状态
  help        显示本帮助

如需带参数启动 sensor_demo，请直接运行 ./sensor_demo <参数>。
开机自启动使用包内默认配置，直接修改 config/sensor_config.yaml 即可生效。
EOF
}

run_foreground() {
    export DEMO_DIR
    . "${DEMO_DIR}/env.sh"
    exec "${DEMO_DIR}/bin/sensor_demo"
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
    nohup "${DEMO_DIR}/bin/sensor_demo" >>"${BOOT_LOG}" 2>&1 &
    echo "sensor_demo autostarted (pid $!)" >>"${BOOT_LOG}"
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
    if pgrep -x sensor_demo >/dev/null 2>&1; then
        info "sensor_demo=running"
    else
        info "sensor_demo=stopped"
    fi
}

case "${1:-}" in
    "")
        run_foreground
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
