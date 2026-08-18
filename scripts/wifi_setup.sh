#!/usr/bin/env bash

# 提供地瓜 X5 Buildroot 板载 WiFi 的交互式 AP/STA 配置入口。
set -u

IFACE="${WIFI_IFACE:-wlan0}"
BASE_DIR="${WIFI_MANAGER_DIR:-/userdata/wifi}"
SCRIPT_PATH="${WIFI_SCRIPT_PATH:-/userdata/wifi_setup.sh}"
STATE_FILE="$BASE_DIR/current.conf"
HOSTAPD_CONF="$BASE_DIR/hostapd.conf"
WPA_CONF="$BASE_DIR/wpa_supplicant.conf"
SCAN_FILE="$BASE_DIR/scan_ssids.txt"
LOG_DIR="$BASE_DIR/logs"
HOSTAPD_LOG="$LOG_DIR/hostapd.log"
DNSMASQ_LOG="$LOG_DIR/dnsmasq.log"
WPA_LOG="$LOG_DIR/wpa_supplicant.log"
AUTOSTART_FILE="${WIFI_AUTOSTART_FILE:-/userdata/startup.sh}"
LEGACY_BOOT_FILE="${WIFI_LEGACY_BOOT_FILE:-/etc/init.d/S41wifi_manager}"
AUTOSTART_MARKER="WIFI_AUTOSTART_MANAGED"
DNSMASQ_PID="$BASE_DIR/dnsmasq.pid"
DEFAULT_AP_SSID="RoboBaton-X5"
DEFAULT_AP_IP="192.168.5.1"
DEFAULT_AP_CHANNEL="1"
DEFAULT_DHCP_START="192.168.5.2"
DEFAULT_DHCP_END="192.168.5.254"

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" = "0" ] || die "请使用 root 执行：$SCRIPT_PATH"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"
}

check_deps() {
    need_cmd ip
    need_cmd iw
    need_cmd hostapd
    need_cmd dnsmasq
    need_cmd wpa_supplicant
    need_cmd wpa_cli
    need_cmd udhcpc
    need_cmd killall
    need_cmd awk
    need_cmd sed
}

ensure_dirs() {
    mkdir -p "$BASE_DIR" "$LOG_DIR" || die "无法创建目录：$BASE_DIR"
    chmod 700 "$BASE_DIR" >/dev/null 2>&1 || true
}

escape_wpa_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    printf '%s' "$value"
}

write_state_kv() {
    local key="$1"
    local value="$2"
    printf '%s=' "$key"
    printf '%q\n' "$value"
}

save_ap_state() {
    local ssid="$1"
    local psk="$2"
    local channel="$3"
    local ap_ip="$4"
    local dhcp_start="$5"
    local dhcp_end="$6"
    local tmp="$STATE_FILE.tmp"

    {
        write_state_kv MODE ap
        write_state_kv IFACE "$IFACE"
        write_state_kv AP_SSID "$ssid"
        write_state_kv AP_PSK "$psk"
        write_state_kv AP_CHANNEL "$channel"
        write_state_kv AP_IP "$ap_ip"
        write_state_kv DHCP_START "$dhcp_start"
        write_state_kv DHCP_END "$dhcp_end"
    } > "$tmp" || die "无法写入配置：$tmp"
    chmod 600 "$tmp" >/dev/null 2>&1 || true
    mv "$tmp" "$STATE_FILE" || die "无法保存配置：$STATE_FILE"
}

save_sta_state() {
    local ssid="$1"
    local psk="$2"
    local tmp="$STATE_FILE.tmp"

    {
        write_state_kv MODE sta
        write_state_kv IFACE "$IFACE"
        write_state_kv STA_SSID "$ssid"
        write_state_kv STA_PSK "$psk"
    } > "$tmp" || die "无法写入配置：$tmp"
    chmod 600 "$tmp" >/dev/null 2>&1 || true
    mv "$tmp" "$STATE_FILE" || die "无法保存配置：$STATE_FILE"
}

load_state() {
    [ -f "$STATE_FILE" ] || die "没有保存的 WiFi 配置：$STATE_FILE"
    # current.conf 由本脚本用 bash %q 生成，只 source 受控配置避免解析任意输入。
    # shellcheck disable=SC1090
    . "$STATE_FILE"
}

valid_ipv4_like() {
    case "$1" in
        *[!0-9.]*|''|*..*|.*|*.) return 1 ;;
        *) return 0 ;;
    esac
}

valid_channel_24g() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) [ "$1" -ge 1 ] 2>/dev/null && [ "$1" -le 13 ] 2>/dev/null ;;
    esac
}

read_default() {
    local prompt="$1"
    local default_value="$2"
    local answer
    read -r -p "$prompt [$default_value]: " answer
    if [ -z "$answer" ]; then
        printf '%s' "$default_value"
    else
        printf '%s' "$answer"
    fi
}

read_secret() {
    local prompt="$1"
    local answer
    read -r -s -p "$prompt" answer
    printf '\n' >&2
    printf '%s' "$answer"
}

confirm() {
    local prompt="$1"
    local answer
    read -r -p "$prompt [y/N]: " answer
    case "$answer" in
        y|Y|yes|YES|Yes) return 0 ;;
        *) return 1 ;;
    esac
}

autostart_is_managed() {
    [ -f "$AUTOSTART_FILE" ] || return 1
    [ -n "$(sed -n "/$AUTOSTART_MARKER/p" "$AUTOSTART_FILE" 2>/dev/null)" ]
}

stop_wifi_processes() {
    # 切换 AP/STA 前释放 wlan0，避免 hostapd/wpa_supplicant/dnsmasq 抢占接口。
    killall hostapd >/dev/null 2>&1 || true
    killall wpa_supplicant >/dev/null 2>&1 || true
    killall udhcpc >/dev/null 2>&1 || true

    if [ -f "$DNSMASQ_PID" ]; then
        local pid
        pid="$(cat "$DNSMASQ_PID" 2>/dev/null || true)"
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
            kill "$pid" >/dev/null 2>&1 || true
        fi
        rm -f "$DNSMASQ_PID" >/dev/null 2>&1 || true
    fi

    # 兼容旧 wifi_init.sh 启动的无 pidfile dnsmasq，避免旧 DHCP 服务继续占用 wlan0。
    killall dnsmasq >/dev/null 2>&1 || true
}

disable_wifi_runtime() {
    # 显式停用 WiFi 功能时，不只杀进程，还要清空地址并关闭 wlan0 射频接口。
    # 不卸载 aic8800 内核模块，避免影响蓝牙/驱动生命周期；需要彻底断电时再手工处理。
    stop_wifi_processes
    ip addr flush dev "$IFACE" >/dev/null 2>&1 || true
    ip link set "$IFACE" down >/dev/null 2>&1 || true
    info "已停用 WiFi：进程已停止，$IFACE 已 down；内核驱动未卸载。"
}

prompt_disable_wifi() {
    disable_wifi_runtime
    if confirm "是否同时关闭开机自动启动 WiFi？"; then
        remove_autostart
    fi
}

reset_interface() {
    ip addr flush dev "$IFACE" >/dev/null 2>&1 || true
    ip link set "$IFACE" down >/dev/null 2>&1 || true
    sleep 1
    ip link set "$IFACE" up || die "无法拉起接口：$IFACE"
    sleep 1
}

write_hostapd_config() {
    local ssid="$1"
    local psk="$2"
    local channel="$3"

    cat > "$HOSTAPD_CONF" <<EOF_AP
interface=$IFACE
driver=nl80211
ssid=$ssid
hw_mode=g
channel=$channel
beacon_int=100
dtim_period=2
max_num_sta=32
auth_algs=1
ignore_broadcast_ssid=0
wmm_enabled=1
ctrl_interface=/var/run/hostapd
ctrl_interface_group=0
EOF_AP

    if [ -n "$psk" ]; then
        cat >> "$HOSTAPD_CONF" <<EOF_AP_SEC
wpa=2
wpa_passphrase=$psk
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF_AP_SEC
    fi

    chmod 600 "$HOSTAPD_CONF" >/dev/null 2>&1 || true
}

write_wpa_config() {
    local ssid="$1"
    local psk="$2"
    local ssid_escaped
    local psk_escaped
    ssid_escaped="$(escape_wpa_string "$ssid")"
    psk_escaped="$(escape_wpa_string "$psk")"

    cat > "$WPA_CONF" <<EOF_WPA
ctrl_interface=/var/run/wpa_supplicant
update_config=1
ap_scan=1
country=CN

network={
    ssid="$ssid_escaped"
EOF_WPA

    if [ -n "$psk" ]; then
        cat >> "$WPA_CONF" <<EOF_WPA_SEC
    psk="$psk_escaped"
    key_mgmt=WPA-PSK
EOF_WPA_SEC
    else
        cat >> "$WPA_CONF" <<EOF_WPA_OPEN
    key_mgmt=NONE
EOF_WPA_OPEN
    fi

    cat >> "$WPA_CONF" <<EOF_WPA_END
}
EOF_WPA_END
    chmod 600 "$WPA_CONF" >/dev/null 2>&1 || true
}

start_ap_runtime() {
    local ssid="$1"
    local psk="$2"
    local channel="$3"
    local ap_ip="$4"
    local dhcp_start="$5"
    local dhcp_end="$6"

    write_hostapd_config "$ssid" "$psk" "$channel"
    stop_wifi_processes
    reset_interface
    ip addr add "$ap_ip/24" dev "$IFACE" || die "无法设置 AP 地址：$ap_ip/24"

    # AP 进程输出写入 /userdata，避免 SSH 远程执行时后台进程继承终端导致命令挂住。
    hostapd -B "$HOSTAPD_CONF" > "$HOSTAPD_LOG" 2>&1 || die "hostapd 启动失败，查看：$HOSTAPD_LOG"
    sleep 1
    dnsmasq --interface="$IFACE" \
        --bind-interfaces \
        --no-resolv \
        --no-poll \
        --dhcp-range="$dhcp_start,$dhcp_end,12h" \
        --dhcp-option="3,$ap_ip" \
        --dhcp-option="6,$ap_ip" \
        --pid-file="$DNSMASQ_PID" \
        > "$DNSMASQ_LOG" 2>&1 &
    sleep 1

    local state
    state="$(wpa_cli -p /var/run/hostapd -i "$IFACE" status 2>/dev/null | awk -F= '$1 == "state" {print $2}')"
    [ "$state" = "ENABLED" ] || die "AP 未进入 ENABLED 状态，查看：$HOSTAPD_LOG"

    info "AP 已启动：SSID=$ssid, IP=$ap_ip, DHCP=$dhcp_start-$dhcp_end"
}

wait_sta_connected() {
    local i
    local state
    for i in $(seq 1 25); do
        state="$(wpa_cli -i "$IFACE" status 2>/dev/null | awk -F= '$1 == "wpa_state" {print $2}')"
        [ "$state" = "COMPLETED" ] && return 0
        sleep 1
    done
    return 1
}

start_sta_runtime() {
    local ssid="$1"
    local psk="$2"

    write_wpa_config "$ssid" "$psk"
    stop_wifi_processes
    reset_interface

    # STA 连接日志持久化，便于现场排查密码错误、认证失败和 DHCP 失败。
    wpa_supplicant -D nl80211 -i "$IFACE" -c "$WPA_CONF" -B > "$WPA_LOG" 2>&1 || die "wpa_supplicant 启动失败，查看：$WPA_LOG"
    if ! wait_sta_connected; then
        iw dev "$IFACE" link 2>/dev/null || true
        die "未连接到 SSID=$ssid，请检查密码或信号，查看：$WPA_LOG"
    fi

    if udhcpc -i "$IFACE" -q -n -t 5 >/dev/null 2>&1; then
        info "STA 已连接并获取 DHCP 地址。"
    else
        warn "STA 已关联，但 DHCP 获取失败；可手动检查路由器或配置静态地址。"
    fi
    ip addr show dev "$IFACE"
}

scan_ssids() {
    reset_interface
    printf '%s\n' "正在扫描 WiFi，请稍等..." >&2
    iw dev "$IFACE" scan 2>/dev/null \
        | awk -F'SSID: ' '/^[[:space:]]*SSID: / { if (length($2) > 0 && !seen[$2]++) print $2 }' \
        > "$SCAN_FILE"

    if [ ! -s "$SCAN_FILE" ]; then
        return 1
    fi

    awk '{ printf "%2d) %s\n", NR, $0 }' "$SCAN_FILE" >&2
    return 0
}

choose_scanned_ssid() {
    local count
    local choice
    local ssid

    if ! scan_ssids; then
        die "未扫描到可见 SSID；请检查天线、距离或隐藏网络。"
    fi

    count="$(awk 'END {print NR}' "$SCAN_FILE")"
    while true; do
        read -r -p "请选择 WiFi 编号，输入 r 重新扫描，输入 q 退出: " choice
        case "$choice" in
            q|Q) exit 0 ;;
            r|R) scan_ssids || die "重新扫描仍未发现可见 SSID"; count="$(awk 'END {print NR}' "$SCAN_FILE")" ;;
            ''|*[!0-9]*) warn "请输入 1-$count 的编号。" ;;
            *)
                if [ "$choice" -ge 1 ] 2>/dev/null && [ "$choice" -le "$count" ] 2>/dev/null; then
                    ssid="$(awk -v n="$choice" 'NR == n {print; exit}' "$SCAN_FILE")"
                    printf '%s' "$ssid"
                    return 0
                fi
                warn "编号超出范围：1-$count"
                ;;
        esac
    done
}

prompt_ap_mode() {
    local ssid
    local psk
    local psk2
    local channel
    local ap_ip
    local dhcp_start
    local dhcp_end

    info "配置 AP 模式。密码留空表示开放热点；非空密码长度必须为 8-63 个字符。"
    ssid="$(read_default "AP SSID" "$DEFAULT_AP_SSID")"
    while true; do
        psk="$(read_secret "AP 密码（可留空）: ")"
        if [ -z "$psk" ]; then
            break
        fi
        if [ "${#psk}" -lt 8 ] || [ "${#psk}" -gt 63 ]; then
            warn "WPA2 密码长度必须为 8-63 个字符。"
            continue
        fi
        psk2="$(read_secret "再次输入 AP 密码: ")"
        [ "$psk" = "$psk2" ] && break
        warn "两次密码不一致。"
    done

    while true; do
        channel="$(read_default "2.4G 信道" "$DEFAULT_AP_CHANNEL")"
        valid_channel_24g "$channel" && break
        warn "信道必须是 1-13。"
    done

    while true; do
        ap_ip="$(read_default "AP IP，固定 /24 掩码" "$DEFAULT_AP_IP")"
        valid_ipv4_like "$ap_ip" && break
        warn "IP 格式不合法。"
    done

    dhcp_start="$(read_default "DHCP 起始地址" "$DEFAULT_DHCP_START")"
    dhcp_end="$(read_default "DHCP 结束地址" "$DEFAULT_DHCP_END")"

    start_ap_runtime "$ssid" "$psk" "$channel" "$ap_ip" "$dhcp_start" "$dhcp_end"
    save_ap_state "$ssid" "$psk" "$channel" "$ap_ip" "$dhcp_start" "$dhcp_end"
    ask_autostart
}

prompt_sta_mode() {
    local ssid
    local psk

    ssid="$(choose_scanned_ssid)"
    info "已选择 SSID：$ssid"
    psk="$(read_secret "WiFi 密码（开放网络可留空）: ")"

    start_sta_runtime "$ssid" "$psk"
    save_sta_state "$ssid" "$psk"
    ask_autostart
}

write_autostart_file() {
    # Buildroot 的 S99auto_startup 会在 /userdata 挂载后执行，这里只写入 late-boot 启动入口。
    # 保留 WiFi 配置在 /userdata/wifi/current.conf，开机后由 userdata 启动钩子恢复。
    cat > "$AUTOSTART_FILE" <<EOF_BOOT
#!/bin/sh
# WIFI_AUTOSTART_MANAGED
# 由 wifi_setup.sh 自动生成，供 S99auto_startup 在 /userdata 挂载后启动 WiFi。
/userdata/wifi_setup.sh --apply-saved >/userdata/wifi/boot.log 2>&1
EOF_BOOT
}

install_autostart() {
    ensure_dirs
    chmod +x "$SCRIPT_PATH" >/dev/null 2>&1 || true

    if [ -f "$AUTOSTART_FILE" ] && ! autostart_is_managed; then
        die "检测到已有 $AUTOSTART_FILE，且不是本脚本生成；请手工合并后再启用。"
    fi

    write_autostart_file || die "无法写入开机启动脚本：$AUTOSTART_FILE"
    chmod 755 "$AUTOSTART_FILE" || die "无法设置开机启动脚本可执行：$AUTOSTART_FILE"

    # 清理旧的 S41 早期启动文件，避免其在 /userdata 挂载前抢跑并造成“开机不生效”。
    rm -f "$LEGACY_BOOT_FILE" >/dev/null 2>&1 || true

    sync
    info "已保存开机启动：$AUTOSTART_FILE"
}

remove_autostart() {
    if [ -f "$AUTOSTART_FILE" ] && autostart_is_managed; then
        rm -f "$AUTOSTART_FILE" >/dev/null 2>&1 || {
            mount -o remount,rw / >/dev/null 2>&1 || true
            rm -f "$AUTOSTART_FILE" || die "无法删除开机启动脚本：$AUTOSTART_FILE"
        }
    fi

    # 旧的 S41 早期启动文件属于过时实现，关闭开机启动时一并清理。
    rm -f "$LEGACY_BOOT_FILE" >/dev/null 2>&1 || true

    sync
    info "已关闭本脚本管理的开机启动。"
}

ask_autostart() {
    if confirm "是否保存/更新为开机自动启动当前 WiFi 配置？"; then
        install_autostart
    else
        remove_autostart
    fi
}

apply_saved() {
    ensure_dirs
    load_state
    IFACE="${IFACE:-wlan0}"
    case "${MODE:-}" in
        ap)
            start_ap_runtime "${AP_SSID:-$DEFAULT_AP_SSID}" "${AP_PSK:-}" "${AP_CHANNEL:-$DEFAULT_AP_CHANNEL}" "${AP_IP:-$DEFAULT_AP_IP}" "${DHCP_START:-$DEFAULT_DHCP_START}" "${DHCP_END:-$DEFAULT_DHCP_END}"
            ;;
        sta)
            [ -n "${STA_SSID:-}" ] || die "STA_SSID 为空，无法启动客户端模式。"
            start_sta_runtime "$STA_SSID" "${STA_PSK:-}"
            ;;
        *)
            die "未知保存模式：${MODE:-empty}"
            ;;
    esac
}

status_wifi() {
    info "=== $IFACE address ==="
    ip addr show dev "$IFACE" 2>/dev/null || true
    info "=== $IFACE info ==="
    iw dev "$IFACE" info 2>/dev/null || true
    info "=== station link ==="
    iw dev "$IFACE" link 2>/dev/null || true
    info "=== hostapd status ==="
    wpa_cli -p /var/run/hostapd -i "$IFACE" status 2>/dev/null || true
    info "=== saved config ==="
    if [ -f "$STATE_FILE" ]; then
        sed -n 's/^MODE=.*/MODE=<saved>/p; s/^AP_SSID=.*/AP_SSID=<saved>/p; s/^STA_SSID=.*/STA_SSID=<saved>/p; s/^AP_IP=.*/AP_IP=<saved>/p' "$STATE_FILE"
    else
        info "none"
    fi
    [ -e "$AUTOSTART_FILE" ] && info "autostart=enabled ($AUTOSTART_FILE)" || info "autostart=disabled"
    [ -e "$LEGACY_BOOT_FILE" ] && info "legacy_autostart=present ($LEGACY_BOOT_FILE)" || info "legacy_autostart=absent"
}

usage() {
    cat <<EOF_USAGE
用法：
  $SCRIPT_PATH                 交互式配置 AP/客户端模式
  $SCRIPT_PATH --apply-saved   按 /userdata/wifi/current.conf 启动已保存配置
  $SCRIPT_PATH --stop          停止 WiFi 进程、清空地址并关闭 wlan0
  $SCRIPT_PATH --disable       同 --stop，不卸载内核驱动
  $SCRIPT_PATH --status        查看 wlan0/保存配置/开机启动状态
  $SCRIPT_PATH --help          显示帮助
EOF_USAGE
}

interactive_menu() {
    while true; do
        cat <<EOF_MENU

请选择 WiFi 模式：
  1) AP 模式：把板子变成热点
  2) 客户端模式：扫描并连接路由器 WiFi
  3) 查看状态
  4) 停用 WiFi（停止服务并关闭 wlan0）
  5) 退出
EOF_MENU
        read -r -p "输入编号: " choice
        case "$choice" in
            1) prompt_ap_mode; return 0 ;;
            2) prompt_sta_mode; return 0 ;;
            3) status_wifi ;;
            4) prompt_disable_wifi; return 0 ;;
            5|q|Q) return 0 ;;
            *) warn "无效选择：$choice" ;;
        esac
    done
}

main() {
    need_root
    check_deps
    ensure_dirs

    case "${1:-}" in
        --apply-saved) apply_saved ;;
        --stop|--disable) disable_wifi_runtime ;;
        --status) status_wifi ;;
        --help|-h) usage ;;
        '') interactive_menu ;;
        *) usage; exit 1 ;;
    esac
}

main "$@"
