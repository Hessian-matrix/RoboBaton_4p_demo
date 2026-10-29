#!/bin/sh
# The immutable getty entry reads this policy on each new debug-console login.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
STATE_DIR=/userdata/uart-autologin
STATE="$STATE_DIR/state"
TMP=""

usage() {
    echo "Usage: uart-autologin {status|enable|disable}"
    echo "Default: enabled on ttyS0. Changes apply at the next login, not to existing sessions."
}

fail() {
    echo "uart-autologin: $*" >&2
    exit 1
}

cleanup() {
    [ -z "$TMP" ] || rm -f "$TMP"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

trusted_userdata() {
    mountpoint -q /userdata && [ ! -L /userdata ] || return 1
    case "$(stat -c '%u:%g:%a' /userdata)" in
        0:0:755|0:0:750|0:0:700|0:0:555|0:0:550|0:0:500) return 0 ;;
        *) return 1 ;;
    esac
}

trusted_state_dir() {
    [ -d "$STATE_DIR" ] && [ ! -L "$STATE_DIR" ] &&
        [ "$(stat -c '%u:%g:%a' "$STATE_DIR")" = '0:0:700' ]
}

get_state() {
    [ "$(id -u)" -eq 0 ] || fail "must run as root"
    trusted_userdata || fail "userdata is not a trusted mounted filesystem; password login required"
    if [ ! -e "$STATE_DIR" ] && [ ! -L "$STATE_DIR" ]; then
        echo enabled
        return
    fi
    trusted_state_dir || fail "untrusted state directory; password login required"
    if [ ! -e "$STATE" ] && [ ! -L "$STATE" ]; then
        echo enabled
        return
    fi
    [ -f "$STATE" ] && [ ! -L "$STATE" ] || fail "invalid state file; password login required"
    [ "$(stat -c '%u:%g:%a' "$STATE")" = '0:0:600' ] ||
        fail "untrusted state file; password login required"
    if printf 'enabled\n' | cmp -s - "$STATE"; then
        echo enabled
    elif printf 'disabled\n' | cmp -s - "$STATE"; then
        echo disabled
    else
        fail "invalid state; password login required"
    fi
}

set_state() {
    [ "$(id -u)" -eq 0 ] || fail "must run as root"
    trusted_userdata || fail "userdata is not a trusted mounted filesystem"
    umask 077
    if [ ! -e "$STATE_DIR" ] && [ ! -L "$STATE_DIR" ]; then
        mkdir -m 0700 "$STATE_DIR" || fail "cannot create state directory"
        chown 0:0 "$STATE_DIR" || fail "cannot own state directory"
    fi
    trusted_state_dir || fail "untrusted state directory"
    if [ -e "$STATE" ] || [ -L "$STATE" ]; then
        [ -f "$STATE" ] && [ ! -L "$STATE" ] || fail "invalid state file"
    fi
    TMP="$(mktemp "$STATE_DIR/.state.XXXXXX")" || fail "cannot prepare state"
    printf '%s\n' "$1" >"$TMP"
    chown 0:0 "$TMP"
    mv -f "$TMP" "$STATE" || fail "cannot install state"
    TMP=""
    sync
    printf '%s\n' "$1"
    echo "Applies at the next ttyS0 login; existing sessions are unchanged." >&2
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
case "$1" in
    status) get_state ;;
    enable) set_state enabled ;;
    disable) set_state disabled ;;
    -h|--help) usage ;;
    *) usage >&2; exit 2 ;;
esac
