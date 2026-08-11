#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

IFACE="${IFACE:-eth0}"
HOST_LIDAR_CIDR="${HOST_LIDAR_CIDR:-192.168.1.12/24}"
LIDAR_IP="${LIDAR_IP:-192.168.1.100}"
VERIFY_TIMEOUT="${VERIFY_TIMEOUT:-30}"
RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
PTP_CFG="${PTP_CFG:-/etc/linuxptp-mid360-master.cfg}"
PTP4L_DEFAULT="${PTP4L_DEFAULT:-/etc/default/ptp4l}"
PHC2SYS_DEFAULT="${PHC2SYS_DEFAULT:-/etc/default/phc2sys}"
ALIAS_UP="${ALIAS_UP:-/etc/network/if-up.d/mid360-ptp-alias}"
ALIAS_DOWN="${ALIAS_DOWN:-/etc/network/if-down.d/mid360-ptp-alias}"
PTP4L_INIT="${PTP4L_INIT:-/etc/init.d/S65ptp4l}"
PHC2SYS_INIT="${PHC2SYS_INIT:-/etc/init.d/S66phc2sys}"
TCPDUMP_ANY="/tmp/mid360_ptp_any_${RUN_ID}.log"
TCPDUMP_LIDAR="/tmp/mid360_ptp_lidar_${RUN_ID}.log"
EXIT_RESULT_PRINTED=0

usage() {
    cat <<'USAGE'
  sh /root/configure_x5_ptp_master.sh [options]

Purpose:
  Run directly on the X5 board. Configure this X5 as a LinuxPTP
  IEEE1588v2 UDP/IP master, then verify synchronization with a PTP slave.
  Defaults use Livox Mid-360 as the example slave.

Options:
  --interface <name>             X5 PTP master interface, default eth0
  --host-lidar-cidr <cidr>       X5 PTP/slave-side IPv4 CIDR, default 192.168.1.12/24
  --lidar-ip <ip>                Example slave IPv4 address, default Mid-360 192.168.1.100
  --verify-timeout <seconds>     Packet/state verification timeout, default 30
  -h, --help                     Show this help

Result:
  RESULT=PASS only when ptp4l is MASTER and PTP packets involving the slave IP are observed.
USAGE
}

log_msg() {
    printf '[x5-ptp-master] %s\n' "$*"
}

warn_msg() {
    printf '[x5-ptp-master] WARN: %s\n' "$*" >&2
}

fail() {
    printf '[x5-ptp-master] ERROR: %s\n' "$*" >&2
    printf 'RESULT=FAIL\n'
    EXIT_RESULT_PRINTED=1
    exit 1
}

on_exit() {
    status="$?"
    if [ "$status" -ne 0 ] && [ "$EXIT_RESULT_PRINTED" -eq 0 ]; then
        printf 'RESULT=FAIL\n'
    fi
}
trap on_exit EXIT

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

validate_positive_integer() {
    value="$1"
    case "$value" in
        ''|*[!0-9]*) fail "invalid positive integer: $value" ;;
        *) [ "$value" -gt 0 ] || fail "invalid positive integer: $value" ;;
    esac
}

validate_ipv4() {
    label="$1"
    value="$2"
    old_ifs="$IFS"
    IFS=.
    # shellcheck disable=SC2086 # BusyBox /bin/sh needs field splitting after IFS assignment.
    set -- $value
    IFS="$old_ifs"
    [ "$#" -eq 4 ] || fail "invalid IPv4 $label: $value"
    for part in "$@"; do
        case "$part" in
            ''|*[!0-9]*) fail "invalid IPv4 $label: $value" ;;
        esac
        [ "$part" -ge 0 ] && [ "$part" -le 255 ] || fail "invalid IPv4 $label: $value"
    done
}

validate_ipv4_cidr() {
    value="$1"
    case "$value" in
        */*) : ;;
        *) fail "invalid IPv4 CIDR: $value" ;;
    esac
    prefix="${value#*/}"
    validate_ipv4 "CIDR address" "${value%/*}"
    case "$prefix" in
        ''|*[!0-9]*) fail "invalid IPv4 CIDR: $value" ;;
    esac
    [ "$prefix" -ge 1 ] && [ "$prefix" -le 32 ] || fail "invalid IPv4 CIDR: $value"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --interface)
            [ "$#" -ge 2 ] || fail "--interface requires a value"
            IFACE="$2"
            shift 2
            ;;
        --host-lidar-cidr)
            [ "$#" -ge 2 ] || fail "--host-lidar-cidr requires a value"
            HOST_LIDAR_CIDR="$2"
            shift 2
            ;;
        --lidar-ip)
            [ "$#" -ge 2 ] || fail "--lidar-ip requires a value"
            LIDAR_IP="$2"
            shift 2
            ;;
        --verify-timeout)
            [ "$#" -ge 2 ] || fail "--verify-timeout requires a value"
            VERIFY_TIMEOUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            EXIT_RESULT_PRINTED=1
            exit 0
            ;;
        *)
            usage >&2
            fail "unknown option: $1"
            ;;
    esac
done

[ "$(id -u)" -eq 0 ] || fail "must run as root"
case "$IFACE" in
    ''|*[!A-Za-z0-9_.:-]*) fail "invalid interface: $IFACE" ;;
esac
validate_ipv4_cidr "$HOST_LIDAR_CIDR"
validate_ipv4 "lidar" "$LIDAR_IP"
validate_positive_integer "$VERIFY_TIMEOUT"
HOST_LIDAR_IP="${HOST_LIDAR_CIDR%/*}"

for cmd in chmod cmp cp date ethtool grep ip mkdir mv pidof pmc ptp4l phc2sys rm sed sleep tcpdump timeout; do
    need_cmd "$cmd"
done
[ -x "$PTP4L_INIT" ] || fail "missing ptp4l init script: $PTP4L_INIT"
[ -x "$PHC2SYS_INIT" ] || fail "missing phc2sys init script: $PHC2SYS_INIT"
[ -d "/sys/class/net/$IFACE" ] || fail "network interface does not exist: $IFACE"

backup_if_needed() {
    target="$1"
    if [ -e "$target" ] || [ -L "$target" ]; then
        backup="${target}.bak.${RUN_ID}"
        cp -p "$target" "$backup" || fail "cannot back up $target"
        log_msg "BACKUP $target -> $backup"
    fi
}

ensure_parent_dir() {
    target="$1"
    dir="${target%/*}"
    [ "$dir" != "$target" ] || return 0
    [ -d "$dir" ] || mkdir -p "$dir" || fail "cannot create directory $dir"
}

install_tmp() {
    tmp="$1"
    target="$2"
    mode="$3"
    if [ -e "$target" ] && cmp -s "$target" "$tmp"; then
        rm -f "$tmp"
        chmod "$mode" "$target" || fail "cannot chmod $target"
        log_msg "UNCHANGED $target"
        return 0
    fi
    backup_if_needed "$target"
    mv -f "$tmp" "$target" || fail "cannot install $target"
    chmod "$mode" "$target" || fail "cannot chmod $target"
    log_msg "UPDATED $target"
}

# 依赖硬件 TX/RX/raw 时间戳；软件时间戳不满足 Mid-360 的同步精度要求。
verify_hardware_timestamping() {
    info="$(ethtool -T "$IFACE" 2>&1 || true)"
    printf '%s\n' "$info" | grep -q 'SOF_TIMESTAMPING_TX_HARDWARE' || fail "$IFACE lacks hardware TX timestamping"
    printf '%s\n' "$info" | grep -q 'SOF_TIMESTAMPING_RX_HARDWARE' || fail "$IFACE lacks hardware RX timestamping"
    printf '%s\n' "$info" | grep -q 'SOF_TIMESTAMPING_RAW_HARDWARE' || fail "$IFACE lacks raw hardware clock timestamping"
    printf '%s\n' "$info" | grep -Eq 'PTP Hardware Clock:[[:space:]]*[0-9]+' || fail "$IFACE has no PTP hardware clock"
    log_msg "$IFACE hardware timestamping verified"
}

# 保留现有地址，只确认或追加 Mid-360 网段地址，避免覆盖现场网络配置。
configure_lidar_address() {
    ip link set "$IFACE" up || fail "cannot bring up $IFACE"
    if ip -o -4 addr show dev "$IFACE" | grep -F " $HOST_LIDAR_CIDR " >/dev/null 2>&1; then
        log_msg "$IFACE already has $HOST_LIDAR_CIDR"
    else
        ip addr add "$HOST_LIDAR_CIDR" dev "$IFACE" 2>/dev/null || true
        ip -o -4 addr show dev "$IFACE" | grep -F " $HOST_LIDAR_CIDR " >/dev/null 2>&1 || fail "cannot add $HOST_LIDAR_CIDR to $IFACE"
        log_msg "added $HOST_LIDAR_CIDR to $IFACE"
    fi
}

# ifupdown 钩子只维护这个地址，不重写 /etc/network/interfaces。
write_alias_hooks() {
    [ -d /etc/network/if-up.d ] || return 0
    tmp="${ALIAS_UP}.tmp.$$"
    cat >"$tmp" <<EOF_UP
#!/bin/sh
[ "\${IFACE:-}" = "$IFACE" ] || exit 0
ip addr add "$HOST_LIDAR_CIDR" dev "$IFACE" 2>/dev/null || true
exit 0
EOF_UP
    install_tmp "$tmp" "$ALIAS_UP" 0755

    [ -d /etc/network/if-down.d ] || return 0
    tmp="${ALIAS_DOWN}.tmp.$$"
    cat >"$tmp" <<EOF_DOWN
#!/bin/sh
[ "\${IFACE:-}" = "$IFACE" ] || exit 0
ip addr del "$HOST_LIDAR_CIDR" dev "$IFACE" 2>/dev/null || true
exit 0
EOF_DOWN
    install_tmp "$tmp" "$ALIAS_DOWN" 0755
}

# X5 作为唯一 LinuxPTP master；LiDAR 作为 slave，配置文件和运行参数必须保持一致。
write_ptp_configs() {
    ensure_parent_dir "$PTP_CFG"
    tmp="${PTP_CFG}.tmp.$$"
    {
        printf '[global]\n'
        printf 'twoStepFlag             1\n'
        printf 'masterOnly              1\n'
        printf 'network_transport       UDPv4\n'
        printf 'delay_mechanism         E2E\n'
        printf 'time_stamping           hardware\n'
        printf 'step_threshold          1.0\n'
        printf '\n[%s]\n' "$IFACE"
    } >"$tmp" || fail "cannot write temporary PTP config"
    install_tmp "$tmp" "$PTP_CFG" 0644

    ensure_parent_dir "$PTP4L_DEFAULT"
    tmp="${PTP4L_DEFAULT}.tmp.$$"
    printf 'PTP4L_ARGS="-f %s -i %s"\n' "$PTP_CFG" "$IFACE" >"$tmp" || fail "cannot write temporary ptp4l defaults"
    install_tmp "$tmp" "$PTP4L_DEFAULT" 0644

    ensure_parent_dir "$PHC2SYS_DEFAULT"
    tmp="${PHC2SYS_DEFAULT}.tmp.$$"
    printf 'PHC2SYS_ARGS="-c %s -s CLOCK_REALTIME -O 0 -S 1.0"\n' "$IFACE" >"$tmp" || fail "cannot write temporary phc2sys defaults"
    install_tmp "$tmp" "$PHC2SYS_DEFAULT" 0644
}

restart_ptp_services() {
    log_msg "restarting ptp4l"
    "$PTP4L_INIT" restart || fail "ptp4l restart failed"
    sleep 1
    pidof ptp4l >/dev/null 2>&1 || fail "ptp4l is not running after restart"

    log_msg "restarting phc2sys"
    "$PHC2SYS_INIT" restart || fail "phc2sys restart failed"
    sleep 1
    pidof phc2sys >/dev/null 2>&1 || fail "phc2sys is not running after restart"
}

wait_for_master_state() {
    deadline=$(( $(date +%s) + VERIFY_TIMEOUT ))
    last_pmc=""
    while [ "$(date +%s)" -le "$deadline" ]; do
        last_pmc="$(pmc -u -b 0 'GET PORT_DATA_SET' 2>&1 || true)"
        if printf '%s\n' "$last_pmc" | grep -q 'portState[[:space:]]*MASTER'; then
            printf '%s\n' "$last_pmc"
            log_msg "PTP4L_STATE=MASTER"
            return 0
        fi
        sleep 1
    done
    printf '%s\n' "$last_pmc"
    fail "ptp4l did not reach MASTER within ${VERIFY_TIMEOUT}s"
}

capture_filter() {
    label="$1"
    filter="$2"
    output="$3"
    rm -f "$output"
    if timeout "$VERIFY_TIMEOUT" tcpdump -i "$IFACE" -nn -c 1 "$filter" >"$output" 2>&1; then
        log_msg "$label=PASS"
        sed -n '1,5p' "$output"
        return 0
    fi
    sed -n '1,20p' "$output" 2>/dev/null || true
    fail "$label=FAIL no packet matched filter: $filter"
}

verify_hardware_timestamping
configure_lidar_address
write_alias_hooks
write_ptp_configs
restart_ptp_services
wait_for_master_state

if ping -c 1 -W 1 "$LIDAR_IP" >/dev/null 2>&1; then
    log_msg "LIDAR_PING=PASS $LIDAR_IP"
else
    warn_msg "LIDAR_PING=UNVERIFIED $LIDAR_IP; continuing because some Mid-360 deployments block ICMP"
fi

capture_filter "PTP_ANY_PACKET" "udp port 319 or udp port 320" "$TCPDUMP_ANY"
capture_filter "PTP_LIDAR_PACKET" "host $LIDAR_IP and (udp port 319 or udp port 320)" "$TCPDUMP_LIDAR"

log_msg "EVIDENCE_PTP_ANY=$TCPDUMP_ANY"
log_msg "EVIDENCE_PTP_LIDAR=$TCPDUMP_LIDAR"
log_msg "HOST_LIDAR_IP=$HOST_LIDAR_IP"
log_msg "LIDAR_IP=$LIDAR_IP"
printf 'RESULT=PASS\n'
EXIT_RESULT_PRINTED=1
