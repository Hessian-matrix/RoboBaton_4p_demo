#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

INITTAB=/etc/inittab
WRAPPER=/bin/autologin-root
STANDARD_GETTY='sole::respawn:/sbin/getty -L  console 0 vt100 # GENERIC_SERIAL'
ENABLED_GETTY='sole::respawn:/sbin/getty -L -n -l /bin/autologin-root 0 console vt100 # GENERIC_SERIAL'
RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
TMP=""
CONTENT_TMP=""
WRAPPER_TMP=""

usage() {
  printf 'Usage: %s {status|enable|disable}\n' "$0"
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  [ -z "$TMP" ] || rm -f "$TMP"
  [ -z "$CONTENT_TMP" ] || rm -f "$CONTENT_TMP"
  [ -z "$WRAPPER_TMP" ] || rm -f "$WRAPPER_TMP"
}
trap cleanup 0 HUP INT TERM

[ "$#" -eq 1 ] || {
  usage >&2
  exit 2
}
ACTION="$1"
case "$ACTION" in
  status|enable|disable) ;;
  *)
    usage >&2
    exit 2
    ;;
esac

[ -f "$INITTAB" ] || fail "missing $INITTAB"

line_count() {
  grep -F -x -c "$1" "$INITTAB" 2>/dev/null || true
}

wrapper_exists() {
  [ -e "$WRAPPER" ] || [ -L "$WRAPPER" ]
}

wrapper_is_valid() {
  [ -f "$WRAPPER" ] && [ ! -L "$WRAPPER" ] && [ -x "$WRAPPER" ] || return 1
  first=""
  second=""
  extra=""
  {
    IFS= read -r first || return 1
    IFS= read -r second || return 1
    if IFS= read -r extra; then
      return 1
    fi
  } <"$WRAPPER"
  [ "$first" = '#!/bin/sh' ] && [ "$second" = 'exec /bin/login -f root' ]
}

get_state() {
  standard_count="$(line_count "$STANDARD_GETTY")"
  enabled_count="$(line_count "$ENABLED_GETTY")"

  if [ "$standard_count" -eq 1 ] && [ "$enabled_count" -eq 0 ] && ! wrapper_exists; then
    printf 'disabled\n'
    return 0
  fi
  if [ "$standard_count" -eq 0 ] && [ "$enabled_count" -eq 1 ] && wrapper_is_valid; then
    printf 'enabled\n'
    return 0
  fi
  printf 'inconsistent\n'
  return 1
}

reload_getty() {
  init q
}

prepare_inittab() {
  direction="$1"
  TMP="${INITTAB}.new.$$"
  CONTENT_TMP="${TMP}.content"
  rm -f "$TMP" "$CONTENT_TMP"
  cp -p "$INITTAB" "$TMP" || fail "cannot prepare $INITTAB"

  case "$direction" in
    enable)
      sed 's|^sole::respawn:/sbin/getty -L  console 0 vt100 # GENERIC_SERIAL$|sole::respawn:/sbin/getty -L -n -l /bin/autologin-root 0 console vt100 # GENERIC_SERIAL|' \
        "$INITTAB" >"$CONTENT_TMP" || fail "cannot prepare enabled getty configuration"
      expected="$ENABLED_GETTY"
      ;;
    disable)
      sed 's|^sole::respawn:/sbin/getty -L -n -l /bin/autologin-root 0 console vt100 # GENERIC_SERIAL$|sole::respawn:/sbin/getty -L  console 0 vt100 # GENERIC_SERIAL|' \
        "$INITTAB" >"$CONTENT_TMP" || fail "cannot prepare disabled getty configuration"
      expected="$STANDARD_GETTY"
      ;;
    *) fail "internal direction error: $direction" ;;
  esac

  [ "$(grep -F -x -c "$expected" "$CONTENT_TMP" 2>/dev/null || true)" -eq 1 ] ||
    fail "prepared getty configuration did not contain the expected line"
  cmp -s "$INITTAB" "$CONTENT_TMP" && fail "getty configuration did not change"
  cat "$CONTENT_TMP" >"$TMP" || fail "cannot preserve $INITTAB metadata"
  rm -f "$CONTENT_TMP"
  CONTENT_TMP=""
}

backup_inittab() {
  BACKUP="${INITTAB}.bak.${RUN_ID}"
  cp -p "$INITTAB" "$BACKUP" || fail "cannot back up $INITTAB"
}

write_wrapper_tmp() {
  WRAPPER_TMP="${WRAPPER}.new.$$"
  rm -f "$WRAPPER_TMP"
  cat >"$WRAPPER_TMP" <<'EOF'
#!/bin/sh
exec /bin/login -f root
EOF
  chmod 0755 "$WRAPPER_TMP" || fail "cannot chmod temporary autologin wrapper"
}

enable_autologin() {
  state="$(get_state || true)"
  if [ "$state" = "enabled" ]; then
    printf 'enabled\n'
    return 0
  fi
  [ "$state" = "disabled" ] ||
    fail "inconsistent or unsupported UART getty configuration; inspect status before enabling"

  prepare_inittab enable
  write_wrapper_tmp
  backup_inittab

  mv -f "$WRAPPER_TMP" "$WRAPPER" || fail "cannot install $WRAPPER"
  WRAPPER_TMP=""
  if ! mv -f "$TMP" "$INITTAB"; then
    rm -f "$WRAPPER"
    fail "cannot install $INITTAB"
  fi
  TMP=""
  sync

  if ! reload_getty; then
    rollback="${INITTAB}.rollback.$$"
    cp -p "$BACKUP" "$rollback" && mv -f "$rollback" "$INITTAB" || true
    rm -f "$WRAPPER"
    sync
    reload_getty || true
    fail "cannot reload getty; restored the previous configuration"
  fi

  state="$(get_state || true)"
  if [ "$state" != "enabled" ]; then
    rollback="${INITTAB}.rollback.$$"
    cp -p "$BACKUP" "$rollback" && mv -f "$rollback" "$INITTAB" || true
    rm -f "$WRAPPER"
    sync
    reload_getty || true
    fail "enable verification failed; restored the previous configuration"
  fi

  printf 'enabled\n'
  printf 'backup: %s\n' "$BACKUP"
}

disable_autologin() {
  standard_count="$(line_count "$STANDARD_GETTY")"
  enabled_count="$(line_count "$ENABLED_GETTY")"

  if [ "$standard_count" -eq 1 ] && [ "$enabled_count" -eq 0 ]; then
    if wrapper_exists; then
      rm -f "$WRAPPER" || fail "cannot remove stale $WRAPPER"
    fi
    printf 'disabled\n'
    return 0
  fi

  [ "$standard_count" -eq 0 ] && [ "$enabled_count" -eq 1 ] ||
    fail "inconsistent or unsupported UART getty configuration; refusing to disable automatically"

  prepare_inittab disable
  backup_inittab
  mv -f "$TMP" "$INITTAB" || fail "cannot install $INITTAB"
  TMP=""
  if wrapper_exists; then
    rm -f "$WRAPPER" || fail "getty was disabled but $WRAPPER could not be removed"
  fi
  sync
  reload_getty || fail "autologin is disabled on disk, but getty reload failed"

  state="$(get_state || true)"
  [ "$state" = "disabled" ] || fail "disable verification failed"
  printf 'disabled\n'
  printf 'backup: %s\n' "$BACKUP"
}

case "$ACTION" in
  status)
    get_state
    ;;
  enable)
    [ "$(id -u)" -eq 0 ] || fail "must run as root"
    enable_autologin
    ;;
  disable)
    [ "$(id -u)" -eq 0 ] || fail "must run as root"
    disable_autologin
    ;;
esac
