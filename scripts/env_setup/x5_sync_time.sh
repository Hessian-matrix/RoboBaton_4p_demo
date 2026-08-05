#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

NTP_INIT="${NTP_INIT:-/etc/init.d/S49ntp}"
PTP4L_INIT="${PTP4L_INIT:-/etc/init.d/S65ptp4l}"
PHC2SYS_INIT="${PHC2SYS_INIT:-/etc/init.d/S66phc2sys}"
RESOLV_FILE="${RESOLV_FILE:-/tmp/resolv.conf}"
TIMEZONE="${TIMEZONE:-Asia/Shanghai}"
TIMEZONE_FILE="${TIMEZONE_FILE:-/etc/timezone}"
LOCALTIME_FILE="${LOCALTIME_FILE:-/etc/localtime}"
ZONEINFO_ROOT="${ZONEINFO_ROOT:-/usr/share/zoneinfo}"
NTP_SERVER="${NTP_SERVER:-0.pool.ntp.org}"
NTP_FALLBACK_SERVER="${NTP_FALLBACK_SERVER:-202.118.1.81}"
DNS_PRIMARY="${DNS_PRIMARY:-223.5.5.5}"
DNS_SECONDARY="${DNS_SECONDARY:-223.6.6.6}"
NTPQ_TIMEOUT_S="${NTPQ_TIMEOUT_S:-90}"
WRITE_RTC=1
ALLOW_UNVERIFIED=0
STOP_PTP4L=0
PHC2SYS_STOPPED=0
PTP4L_STOPPED=0
NTPD_STOPPED=0
NTPD_STARTED_BY_SCRIPT=0
KEEP_CURRENT_OWNER=0
resolver_tmp=""
timezone_tmp=""
localtime_tmp=""
timezone_backup=""
localtime_backup=""
TIMEZONE_TRANSACTION_ACTIVE=0
TIMEZONE_OLD_MOVED=0
LOCALTIME_OLD_MOVED=0
TIMEZONE_INSTALLED=0
LOCALTIME_INSTALLED=0
NTPDATE_BIN="${NTPDATE_BIN:-$(command -v ntpdate 2>/dev/null || true)}"
PIDOF_BIN="${PIDOF_BIN:-$(command -v pidof 2>/dev/null || true)}"
TIMEZONE_MV_BIN="${TIMEZONE_MV_BIN:-$(command -v mv 2>/dev/null || true)}"
HWCLOCK_BIN="${HWCLOCK_BIN:-$(command -v hwclock 2>/dev/null || true)}"

usage() {
    cat <<'USAGE'
Usage:
  x5_sync_time.sh [options]

Purpose:
  Synchronize the X5 system clock from NTP without claiming daemon lock
  unless ntpq reports a selected (*) peer.

Options:
  --server <host-or-ip>       Primary one-shot NTP source.
  --fallback-server <host-ip> Numeric fallback NTP source.
  --dns <primary,secondary>   Runtime DNS servers; default 223.5.5.5,223.6.6.6.
  --resolv-file <path>        Runtime resolver file; default /tmp/resolv.conf.
  --ntpq-timeout <seconds>    Peer-lock wait limit; default 90.
  --no-rtc                    Do not write the successful system time to RTC.
  --stop-ptp4l              Also stop ptp4l; disabled by default because ptp4l does not write CLOCK_REALTIME.
  --allow-unverified          Return 0 when one-shot sync succeeds but ntpq is absent.
  -h, --help                  Show this help.

Environment:
  TIMEZONE, TIMEZONE_FILE, LOCALTIME_FILE, and ZONEINFO_ROOT may override
  the default Asia/Shanghai timezone paths for controlled deployments/tests.
  TIMEZONE_MV_BIN may override mv for controlled failure testing.
  HWCLOCK_BIN may override hwclock for controlled RTC verification tests.

Exit codes:
  0  System time synced and ntpq confirmed a selected (*) peer, or
     --allow-unverified was supplied.
  1  Dependency, service, network, NTP, or RTC operation failed.
  2  One-shot sync succeeded, but ntpq is unavailable and daemon lock is unverified.
  3  ntpq is available, but no selected (*) peer appeared before timeout.
USAGE
}

log_msg() {
    printf '[x5-sync] %s\n' "$*"
}

warn_msg() {
    printf '[x5-sync] WARN: %s\n' "$*" >&2
}

fail() {
    printf '[x5-sync] ERROR: %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

is_running() {
    "$PIDOF_BIN" "$1" >/dev/null 2>&1
}

# 2026-07-26：校时失败时恢复脚本接管前的时钟服务状态，避免失败路径中断原有 PTP/相机时间链路。
restore_services() {
    if [ "$NTPD_STARTED_BY_SCRIPT" -eq 1 ] && is_running ntpd; then
        "$NTP_INIT" stop >/dev/null 2>&1 || warn_msg "failed to stop newly started ntpd during rollback"
    fi

    if [ "$NTPD_STOPPED" -eq 1 ] && is_running ntpd; then
        "$NTP_INIT" stop >/dev/null 2>&1 || warn_msg "failed to stop ntpd before PTP rollback"
    fi
    if is_running ntpd; then
        warn_msg "cannot restore PTP services while ntpd is still running"
        return 0
    fi

    if [ "$PTP4L_STOPPED" -eq 1 ] && ! is_running ptp4l; then
        "$PTP4L_INIT" start >/dev/null 2>&1 || warn_msg "failed to restore ptp4l"
    fi
    if [ "$PHC2SYS_STOPPED" -eq 1 ] && ! is_running phc2sys; then
        "$PHC2SYS_INIT" start >/dev/null 2>&1 || warn_msg "failed to restore phc2sys"
    fi
    if [ "$NTPD_STOPPED" -eq 1 ] && ! is_running ntpd; then
        "$NTP_INIT" start >/dev/null 2>&1 || warn_msg "failed to restore ntpd"
    fi
}

# 2026-07-26：仅清理已确认不再需要恢复的备份；恢复失败时保留路径供人工恢复。
cleanup_timezone_backups() {
    if [ "$TIMEZONE_OLD_MOVED" -eq 0 ] && [ -n "$timezone_backup" ]; then
        if [ -e "$timezone_backup" ] || [ -L "$timezone_backup" ]; then
            rm -f "$timezone_backup" || warn_msg "timezone backup retained at $timezone_backup"
        fi
    fi
    if [ "$LOCALTIME_OLD_MOVED" -eq 0 ] && [ -n "$localtime_backup" ]; then
        if [ -e "$localtime_backup" ] || [ -L "$localtime_backup" ]; then
            rm -f "$localtime_backup" || warn_msg "localtime backup retained at $localtime_backup"
        fi
    fi
}

# 2026-07-26：时区文件采用备份、安装、失败恢复事务，避免两个目标只更新一个。
restore_timezone() {
    [ "$TIMEZONE_TRANSACTION_ACTIVE" -eq 1 ] || return 0

    if [ "$TIMEZONE_OLD_MOVED" -eq 1 ] || [ "$TIMEZONE_INSTALLED" -eq 1 ]; then
        rm -f "$TIMEZONE_FILE" || warn_msg "failed to remove partial timezone file"
    fi
    if [ "$LOCALTIME_OLD_MOVED" -eq 1 ] || [ "$LOCALTIME_INSTALLED" -eq 1 ]; then
        rm -f "$LOCALTIME_FILE" || warn_msg "failed to remove partial localtime link"
    fi
    if [ "$TIMEZONE_OLD_MOVED" -eq 1 ]; then
        if "$TIMEZONE_MV_BIN" -f "$timezone_backup" "$TIMEZONE_FILE"; then
            TIMEZONE_OLD_MOVED=0
        else
            warn_msg "timezone backup retained at $timezone_backup"
        fi
    fi
    if [ "$LOCALTIME_OLD_MOVED" -eq 1 ]; then
        if "$TIMEZONE_MV_BIN" -f "$localtime_backup" "$LOCALTIME_FILE"; then
            LOCALTIME_OLD_MOVED=0
        else
            warn_msg "localtime backup retained at $localtime_backup"
        fi
    fi
    if [ "$TIMEZONE_OLD_MOVED" -eq 0 ] && [ "$LOCALTIME_OLD_MOVED" -eq 0 ]; then
        TIMEZONE_TRANSACTION_ACTIVE=0
    fi
    cleanup_timezone_backups
}

on_exit() {
    exit_status="$?"
    trap - 0 1 2 3 15
    if [ "$exit_status" -ne 0 ] && [ "$KEEP_CURRENT_OWNER" -eq 0 ]; then
        warn_msg "operation failed; restoring pre-run service state"
        restore_timezone
        restore_services
    fi
    [ -z "${resolver_tmp:-}" ] || rm -f "$resolver_tmp"
    [ -z "${timezone_tmp:-}" ] || rm -f "$timezone_tmp"
    [ -z "${localtime_tmp:-}" ] || rm -f "$localtime_tmp"
    cleanup_timezone_backups
    exit "$exit_status"
}

trap 'on_exit' 0
trap 'exit 1' 1 2 3 15

validate_host() {
    value="$1"
    case "$value" in
        ''|-[A-Za-z0-9]*) fail "invalid NTP host or address: $value" ;;
        *[!A-Za-z0-9._:-]*) fail "invalid NTP host or address: $value" ;;
    esac
}

validate_dns() {
    value="$1"
    case "$value" in
        ''|*[!0-9.]*) fail "invalid DNS IPv4 address: $value" ;;
    esac
}

validate_positive_integer() {
    value="$1"
    case "$value" in
        ''|*[!0-9]*) fail "expected a non-negative integer: $value" ;;
    esac
}

validate_timezone() {
    value="$1"
    case "$value" in
        ''|/*|*..*|*[!A-Za-z0-9._/-]*) fail "invalid timezone: $value" ;;
    esac
}

parse_dns_pair() {
    dns_pair="$1"
    case "$dns_pair" in
        *,*)
            DNS_PRIMARY="${dns_pair%%,*}"
            DNS_SECONDARY="${dns_pair#*,}"
            ;;
        *)
            fail "--dns expects primary,secondary"
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --server)
            [ "$#" -ge 2 ] || fail "--server requires a value"
            NTP_SERVER="$2"
            shift 2
            ;;
        --fallback-server)
            [ "$#" -ge 2 ] || fail "--fallback-server requires a value"
            NTP_FALLBACK_SERVER="$2"
            shift 2
            ;;
        --dns)
            [ "$#" -ge 2 ] || fail "--dns requires a value"
            parse_dns_pair "$2"
            shift 2
            ;;
        --resolv-file)
            [ "$#" -ge 2 ] || fail "--resolv-file requires a value"
            RESOLV_FILE="$2"
            shift 2
            ;;
        --ntpq-timeout)
            [ "$#" -ge 2 ] || fail "--ntpq-timeout requires a value"
            NTPQ_TIMEOUT_S="$2"
            shift 2
            ;;
        --no-rtc)
            WRITE_RTC=0
            shift
            ;;
        --stop-ptp4l)
            STOP_PTP4L=1
            shift
            ;;
        --allow-unverified)
            ALLOW_UNVERIFIED=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            fail "unknown option: $1"
            ;;
    esac
done

[ "$(id -u)" -eq 0 ] || fail "must run as root"
validate_host "$NTP_SERVER"
validate_host "$NTP_FALLBACK_SERVER"
validate_dns "$DNS_PRIMARY"
validate_dns "$DNS_SECONDARY"
validate_positive_integer "$NTPQ_TIMEOUT_S"
validate_timezone "$TIMEZONE"
TIMEZONE_PATH="$ZONEINFO_ROOT/$TIMEZONE"
need_cmd date
need_cmd mv
need_cmd mktemp
need_cmd grep
need_cmd sleep
need_cmd chmod
need_cmd dirname
need_cmd rm
need_cmd ln
[ -n "$NTPDATE_BIN" ] && [ -x "$NTPDATE_BIN" ] || fail "missing executable ntpdate: ${NTPDATE_BIN:-unset}"
[ -n "$PIDOF_BIN" ] && [ -x "$PIDOF_BIN" ] || fail "missing executable pidof: ${PIDOF_BIN:-unset}"
[ -x "$NTP_INIT" ] || fail "missing NTP init script: $NTP_INIT"
[ -n "$TIMEZONE_MV_BIN" ] && [ -x "$TIMEZONE_MV_BIN" ] || fail "missing executable timezone mv: ${TIMEZONE_MV_BIN:-unset}"
[ -d "$(dirname "$RESOLV_FILE")" ] || fail "resolver directory does not exist: $(dirname "$RESOLV_FILE")"
[ -d "$(dirname "$TIMEZONE_FILE")" ] || fail "timezone directory does not exist: $(dirname "$TIMEZONE_FILE")"
[ -d "$(dirname "$LOCALTIME_FILE")" ] || fail "localtime directory does not exist: $(dirname "$LOCALTIME_FILE")"
[ -f "$TIMEZONE_PATH" ] || fail "timezone data does not exist: $TIMEZONE_PATH"

if is_running cam-service; then
    warn_msg "cam-service is running; system time may step while it remains active"
fi

# 2026-07-26：只写入运行时 resolver，保持脚本可重复运行且不把临时 DNS 误当成持久配置。
write_runtime_resolver() {
    resolver_tmp="${RESOLV_FILE}.x5-sync.$$"
    printf '%s\n' \
        "nameserver ${DNS_PRIMARY}" \
        "nameserver ${DNS_SECONDARY}" > "$resolver_tmp" || fail "cannot write resolver temporary file"
    chmod 0644 "$resolver_tmp" || fail "cannot chmod resolver temporary file"
    mv -f "$resolver_tmp" "$RESOLV_FILE" || fail "cannot install runtime resolver file"
    resolver_tmp=""
    log_msg "runtime DNS configured at $RESOLV_FILE"
}


# 2026-07-26：NTP 校时成功后统一设置本地时区；NTP 和 RTC 仍使用 UTC 时间基准。
configure_timezone() {
    timezone_tmp="${TIMEZONE_FILE}.x5-sync.$$"
    localtime_tmp="${LOCALTIME_FILE}.x5-sync.$$"
    timezone_backup="${TIMEZONE_FILE}.x5-sync-backup.$$"
    localtime_backup="${LOCALTIME_FILE}.x5-sync-backup.$$"
    TIMEZONE_TRANSACTION_ACTIVE=1

    printf '%s\n' "$TIMEZONE" > "$timezone_tmp" || fail "cannot write timezone temporary file"
    chmod 0644 "$timezone_tmp" || fail "cannot chmod timezone temporary file"
    ln -s "$TIMEZONE_PATH" "$localtime_tmp" || fail "cannot create localtime temporary link"

    if [ -e "$TIMEZONE_FILE" ] || [ -L "$TIMEZONE_FILE" ]; then
        "$TIMEZONE_MV_BIN" -f "$TIMEZONE_FILE" "$timezone_backup" || fail "cannot back up timezone file"
        TIMEZONE_OLD_MOVED=1
    fi
    if [ -e "$LOCALTIME_FILE" ] || [ -L "$LOCALTIME_FILE" ]; then
        "$TIMEZONE_MV_BIN" -f "$LOCALTIME_FILE" "$localtime_backup" || fail "cannot back up localtime link"
        LOCALTIME_OLD_MOVED=1
    fi

    "$TIMEZONE_MV_BIN" -f "$timezone_tmp" "$TIMEZONE_FILE" || fail "cannot install timezone file"
    TIMEZONE_INSTALLED=1
    "$TIMEZONE_MV_BIN" -f "$localtime_tmp" "$LOCALTIME_FILE" || fail "cannot install localtime link"
    LOCALTIME_INSTALLED=1

    TIMEZONE_TRANSACTION_ACTIVE=0
    TIMEZONE_OLD_MOVED=0
    LOCALTIME_OLD_MOVED=0
    cleanup_timezone_backups
    log_msg "timezone configured as $TIMEZONE"
}

# 2026-07-26：ntpd 必须在一次性校时成功后重新接管，失败时由退出 trap 恢复原 owner。
start_ntpd() {
    if is_running ntpd; then
        return 0
    fi
    log_msg "starting ntpd"
    "$NTP_INIT" start >/dev/null || fail "failed to start ntpd"
    is_running ntpd || fail "ntpd did not stay running"
    NTPD_STARTED_BY_SCRIPT=1
}
# 2026-07-26：ntpdate 先完成可观测的一次性校时，避免未校准就写 RTC 或启动新的 owner。
sync_once() {
    server="$1"
    log_msg "one-shot NTP sync from $server"
    if "$NTPDATE_BIN" -u -b "$server"; then
        return 0
    fi
    warn_msg "one-shot NTP sync failed for $server"
    return 1
}
# 2026-07-26：RTC 使用 BusyBox 的 UTC 写入参数，并在 TZ=UTC 环境下回读校验，避免本地时区格式化造成误判。
write_rtc_if_requested() {
    if [ "$WRITE_RTC" -eq 0 ]; then
        log_msg "RTC write disabled"
        return 0
    fi
    if [ -z "$HWCLOCK_BIN" ] || [ ! -x "$HWCLOCK_BIN" ]; then
        warn_msg "hwclock is unavailable; RTC was not updated"
        return 0
    fi
    if "$HWCLOCK_BIN" -w -u; then
        rtc_readback="$(TZ=UTC "$HWCLOCK_BIN" -r -u 2>/dev/null || true)"
        rtc_epoch="$(date -u -d "$rtc_readback" +%s 2>/dev/null || true)"
        system_epoch="$(date -u +%s)"
        if [ -z "$rtc_epoch" ]; then
            fail "RTC UTC readback is unavailable"
        fi
        rtc_delta=$((rtc_epoch - system_epoch))
        if [ "$rtc_delta" -lt -1 ] || [ "$rtc_delta" -gt 1 ]; then
            fail "RTC UTC readback differs from system UTC by ${rtc_delta}s"
        fi
        log_msg "RTC updated and verified as UTC (delta ${rtc_delta}s)"
    else
        warn_msg "system time is synchronized, but RTC update failed"
    fi
}

check_ntpq_lock() {
    ntpq_bin="$(command -v ntpq 2>/dev/null || true)"
    if [ -z "$ntpq_bin" ]; then
        warn_msg "ntpq is unavailable; continuous daemon lock cannot be proven"
        return 2
    fi

    deadline=$(( $(date +%s) + NTPQ_TIMEOUT_S ))
    peers=""
    while [ "$(date +%s)" -lt "$deadline" ]; do
        peers="$($ntpq_bin -pn 2>/dev/null || true)"
        if printf '%s\n' "$peers" | grep -Eq '^[[:space:]]*\*'; then
            printf '%s\n' "$peers"
            log_msg "ntpq confirmed a selected (*) peer"
            return 0
        fi
        sleep 2
    done

    printf '%s\n' "$peers"
    warn_msg "ntpq is available, but no selected (*) peer appeared within ${NTPQ_TIMEOUT_S}s"
    return 3
}

# 2026-07-26：默认只停 phc2sys；ptp4l 不直接写 CLOCK_REALTIME，只有显式选项才停止它。
log_msg "starting NTP-owner synchronization"
if is_running phc2sys; then
    PHC2SYS_STOPPED=1
    log_msg "stopping phc2sys"
    "$PHC2SYS_INIT" stop >/dev/null || fail "failed to stop phc2sys"
fi
if [ "$STOP_PTP4L" -eq 1 ] && is_running ptp4l; then
    PTP4L_STOPPED=1
    log_msg "stopping ptp4l by explicit request"
    "$PTP4L_INIT" stop >/dev/null || fail "failed to stop ptp4l"
fi
if is_running ntpd; then
    NTPD_STOPPED=1
    log_msg "stopping ntpd"
    "$NTP_INIT" stop >/dev/null || fail "failed to stop ntpd"
fi
write_runtime_resolver

if ! sync_once "$NTP_SERVER"; then
    if [ "$NTP_FALLBACK_SERVER" = "$NTP_SERVER" ] || ! sync_once "$NTP_FALLBACK_SERVER"; then
        start_ntpd
        fail "one-shot NTP synchronization failed"
    fi
fi

log_msg "system time after one-shot sync: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
write_rtc_if_requested
configure_timezone
start_ntpd
KEEP_CURRENT_OWNER=1

if check_ntpq_lock; then
    peer_check_status=0
else
    peer_check_status=$?
fi
if [ "$peer_check_status" -eq 0 ]; then
    log_msg "SUCCESS: system time synced and ntpd peer lock verified"
    exit 0
fi
if [ "$peer_check_status" -eq 2 ] && [ "$ALLOW_UNVERIFIED" -eq 1 ]; then
    warn_msg "SUCCESS: one-shot system sync completed; ntpd peer lock remains unverified"
    exit 0
fi
if [ "$peer_check_status" -eq 2 ]; then
    warn_msg "one-shot system sync completed; rerun with --allow-unverified or install ntpq"
    exit 2
fi
exit 3
