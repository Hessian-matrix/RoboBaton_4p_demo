#!/bin/bash
# X5 md-v0p2:把 UART7 RX 脚在「PPS 输入」和「普通串口」之间来回切换
# (在板上以 root 运行;不改 DTS、不烧写;立刻生效,且★切一次就一直保持,重启多少次都不变,
#  直到下一次自己再切。任何一步失败 trap 兜底恢复。)
#
# 用法:
#   x5-pps-pin.sh --check                      # 只探测环境+打印计划,不动任何东西
#   x5-pps-pin.sh pps  [--edge rising|falling|both] [--keep-uart] [--force] [--once]
#   x5-pps-pin.sh uart [--force] [--once]   # 还原成普通串口
#   x5-pps-pin.sh status                       # mux / 模块 / /dev/pps* / 中断计数 / 持久化状态
#   x5-pps-pin.sh test [--secs 10]             # 验真:中断计数和 PPS 序号到底涨不涨
#   x5-pps-pin.sh install                      # 只装开机钩子(pps/uart 已自动装,一般用不上)
#   x5-pps-pin.sh uninstall                    # 摘钩子+删持久配置,彻底恢复出厂开机行为
#   x5-pps-pin.sh boot                         # 开机钩子内部调用,不要手动跑
#
# ★持久化怎么做到的(不改源码树、不改根分区、不重烧固件):
#   模式记在 <脚本所在目录>/mode,开机钩子写进 /userdata/startup.sh —— 这是厂商自带的
#   扩展点,buildroot 与 jammy 两种 rootfs 的 /etc/init.d/S99auto_startup 逐字节相同,
#   都会在启动末尾执行它,所以 SysV / systemd 通吃,且根分区一个字节都不用动
#   (本树 HR_SYSTEM_VERIFY="dm-verity",往 /etc 写有砖化风险,故意绕开)。
#   /userdata 是独立 ext4 分区,重刷固件也不清,所以配置比固件还耐久。
#   `--once` = 只改这一次,不动持久配置。救命开关:touch <脚本目录>/DISABLE,开机即不动手。
#
# 依赖:同目录(或 $X5PPS_KO)下的 x5pps.ko,vermagic 必须与板上 uname -r 一致。
#      要开机自动生效,脚本和 ko 必须放在重启后还在的路径(推荐 /userdata/x5-pps/)。
#
# ★两个必须知道的假阳性:
#   1) 板上本来就有个 /dev/pps0,是假的。kernel/drivers/pps/clients/hobot-pps.c:25 的
#      REAL_PPS_ENABLE 被注释掉,跑的是 1 秒 mod_timer 伪造事件,跟任何引脚无关;
#      而 x5-rdk.dtsi:869 把它 status="okay" 了。认 name == x5pps 的那个,别认 hobot-pps.-1。
#   2) gpio value 读回不可信(dwapb EXT 输入门控)。判 mux 只能看寄存器回读和
#      /sys/kernel/debug/pinctrl/*/pinmux-pins;判"通没通"只能看 /proc/interrupts。
#
# 寄存器表来源:kernel/arch/arm64/boot/dts/hobot/pinmux-gpio.dtsi + pinmux-func.dtsi
#   LSIO iomuxc 基址 0x34180000,LSIO_PINMUX_3 = +0x84;UART 功能 = MUX_ALT0(0),GPIO = MUX_ALT2(2)
# mux 后端(detect_mux_backend/func_name/reg_read/reg_write/reg_rmw)复制自
#   scripts/gpio/header-gpio-test.sh —— 那套已在本板实测 17/17 PASS,含厂商 pinmux-functions
#   双行格式解析和 "<组名> <函数名>" 顺序兜底两个已知坑。此处复制而非 source,是为了单文件 scp。

set -u
LOGTAG="[x5-pps]"
STATE=/var/run/x5-pps.state
[ -d /var/run ] && [ -w /var/run ] 2>/dev/null || STATE=/tmp/x5-pps.state
MODNAME=x5pps
UART_DRV=/sys/bus/platform/drivers/dw-apb-uart

# ---------- 持久化(切一次就一直保持,重启不复位) ----------
# 落点选 /userdata,不选 /etc:本树 device/horizon/x5/*_config.mk 里
# HR_SYSTEM_VERIFY="dm-verity" —— 根分区受 verity 保护,往里写有砖化风险
# (verity_corrupted 两轮就砖);而 /userdata 是独立 ext4 分区,可写,且重刷固件也不动它。
# 开机钩子借用官方扩展点 /userdata/startup.sh:buildroot 与 jammy 两种 rootfs 的
# /etc/init.d/S99auto_startup 内容逐字节相同,都会在启动末尾执行它(可执行即跑),
# 所以不必判 init 体系(SysV / systemd 通吃),也一个字节都不用改根分区。
# 它排在 S65mountall(挂 /userdata)和 S70loadko(装驱动)之后,时序天然正确。
SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || SELFDIR=$(pwd)
PERSIST="$SELFDIR/mode"           # 持久模式文件(MODE=pps|uart)
DISABLE="$SELFDIR/DISABLE"        # 救命开关:此文件存在则开机钩子一律不动手
BOOTLOG="$SELFDIR/boot.log"       # 开机钩子日志
STARTUP=${X5PPS_STARTUP:-/userdata/startup.sh}   # 可覆盖:便于离线自测
HOOK_BEGIN="# >>> x5-pps boot hook >>>"
HOOK_END="# <<< x5-pps boot hook <<<"

# 全局号|排针|信号|gpio组|mux寄存器|bit|GPIO模式值|属主uart|uart组|平台设备|备注
PINS=(
 "379|pin10|LSIO_UART7_RX|lsio_gpio0_0|0x34180084|4|2|uart7|uart7grp|34060000.serial|默认 PPS 输入脚;仅支持 UART7 RX 路径"
)
DEFAULT_PIN=379
# PPS 模式下 UART7 TX 作为内部 GPIO/IO 伴随线释放；不作为 PPS 选择参数暴露。
IO_GPIO=380
IO_HDR=pin8
IO_SIG=LSIO_UART7_TX
IO_GRP=lsio_gpio0_1
IO_ADDR=0x34180084
IO_BIT=6
IO_ALT=2

CMD="" PIN="" EDGE=rising SECS=10 FORCE=0 KEEP_UART=0 ONCE=0
CONSOLE_TTY=""   # console_is_on 命中时填,供提示用
while [ $# -gt 0 ]; do case "$1" in
  pps|uart|status|test|boot|install|uninstall) CMD=$1;;
  --check)      CMD=check;;
  --pin)        PIN=$2; shift;;
  --edge)       EDGE=$2; shift;;
  --secs)       SECS=$2; shift;;
  --keep-uart)  KEEP_UART=1;;
  --once)       ONCE=1;;
  --force)      FORCE=1;;
  -h|--help)    awk 'NR>1{ if(/^#/) print; else exit }' "$0"; exit 0;;
  *) echo "unknown arg: $1" >&2; exit 1;;
esac; shift; done
[ -n "$CMD" ] || { awk 'NR>1{ if(/^#/) print; else exit }' "$0"; exit 1; }

log(){ echo "$LOGTAG $*"; }
die(){ echo "$LOGTAG FATAL: $*" >&2; exit 1; }

# ---------- 引脚表查询 ----------
ROW=""
pin_lookup(){ # 全局号 → 填 ROW / P_* 变量;查不到返回 1 且不留旧值
  local want=$1 r
  ROW=""                      # 必须清:否则查不到时会沿用上一次的行,还原会写错寄存器
  for r in "${PINS[@]}"; do
    if [ "${r%%|*}" = "$want" ]; then ROW=$r; break; fi
  done
  [ -n "$ROW" ] || return 1
  IFS='|' read -r P_GPIO P_HDR P_SIG P_GRP P_ADDR P_BIT P_ALT P_UART P_UGRP P_DEV P_NOTE <<<"$ROW"
  return 0
}
pin_list(){ local r; for r in "${PINS[@]}"; do echo "${r%%|*}"; done; }

# ---------- mux 后端(复制自 header-gpio-test.sh,板上 17/17 PASS 验证过) ----------
MUX_BACKEND="" REG_TOOL="" LSIO_DBG="" LSIO_FUNC=""

func_name(){ # debugfs目录 → 第一个函数名
  # 兼容两种 pinmux-functions 格式:
  #   主线:      "function 0: lsio_iomuxc, groups = [ ... ]"
  #   HOBOT厂商: "function 0: lsio_iomuxc"(组列表另起一行)
  sed -n 's/^function[ 0-9]*: *\([^,]*\).*/\1/p' "$1/pinmux-functions" 2>/dev/null | head -1 | tr -d ' \t\r'
}

detect_mux_backend(){
  mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>/dev/null
  LSIO_DBG=$(ls -d /sys/kernel/debug/pinctrl/*lsio_iomuxc* 2>/dev/null | head -1)
  if [ -n "$LSIO_DBG" ] && [ -w "$LSIO_DBG/pinmux-select" ]; then
    LSIO_FUNC=$(func_name "$LSIO_DBG")
    [ -n "$LSIO_FUNC" ] && MUX_BACKEND=pinmux-select
  fi
  if [ -e /dev/mem ]; then
    if   command -v devmem  >/dev/null 2>&1; then REG_TOOL=devmem
    elif command -v hexdump >/dev/null 2>&1; then REG_TOOL=hexdump-dd
    elif command -v python3 >/dev/null 2>&1; then REG_TOOL=python3
    fi
  fi
  [ -z "$MUX_BACKEND" ] && [ -n "$REG_TOOL" ] && MUX_BACKEND=register
  [ -z "$MUX_BACKEND" ] && die "无可用 mux 后端:pinmux-select 不可用(缺 debugfs 或函数名解析失败),寄存器直写也无工具(devmem/hexdump/python3 + /dev/mem)"
  log "mux 后端: $MUX_BACKEND(寄存器工具: ${REG_TOOL:-无})${LSIO_FUNC:+ lsio函数=$LSIO_FUNC}"
  # 本脚本的状态保存/回读校验都依赖寄存器读,没有就只能盲切
  [ -z "$REG_TOOL" ] && log "警告:没有寄存器读写工具,无法保存原值/回读校验,只能盲切(建议装 devmem)"
}

reg_read(){ # addr → 十进制值到 stdout
  case "$REG_TOOL" in
    devmem)     local h; h=$(devmem "$1" 32) || return 1; echo $((h));;
    hexdump-dd) local h; h=$(hexdump -n4 -s $(($1)) -e '1/4 "%08x"' /dev/mem 2>/dev/null)
                [ -n "$h" ] || return 1; echo $((0x$h));;
    python3)    python3 -c "
import mmap,os
a=$(($1)); pg=a&~0xfff
f=os.open('/dev/mem',os.O_RDONLY|os.O_SYNC)
m=mmap.mmap(f,0x1000,mmap.MAP_SHARED,mmap.PROT_READ,offset=pg)
print(int.from_bytes(m[a-pg:a-pg+4],'little'))";;
    *) return 1;;
  esac
}

reg_write(){ # addr val(十进制)
  case "$REG_TOOL" in
    devmem)     devmem "$1" 32 "$2";;
    hexdump-dd) # 4 字节小端一次性写入(bs=4 保证 32bit 访问,寄存器地址均 4 对齐)
                local v=$2 fmt
                fmt=$(printf '\\%03o\\%03o\\%03o\\%03o' $((v&255)) $(((v>>8)&255)) $(((v>>16)&255)) $(((v>>24)&255)))
                printf "$fmt" | dd of=/dev/mem bs=4 seek=$(($1/4)) count=1 conv=notrunc 2>/dev/null \
                  || printf "$fmt" | dd of=/dev/mem bs=4 seek=$(($1/4)) count=1 2>/dev/null;;
    python3)    python3 - "$(($1))" "$2" <<'PYEOF'
import mmap,os,sys
a=int(sys.argv[1]); v=int(sys.argv[2]); pg=a&~0xfff
f=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
m=mmap.mmap(f,0x1000,offset=pg)
m[a-pg:a-pg+4]=v.to_bytes(4,'little'); m.close(); os.close(f)
PYEOF
;;
    *) return 1;;
  esac
}

reg_rmw(){ # addr bit val  (2bit 字段读改写 + 回读校验)
  local addr=$1 bit=$2 val=$3 cur new
  [ -n "$REG_TOOL" ] || return 1
  cur=$(reg_read "$addr") || return 1
  new=$(( (cur & ~(3<<bit)) | (val<<bit) ))
  reg_write "$addr" "$new" || return 1
  cur=$(reg_read "$addr") || return 0
  [ $(( (cur>>bit) & 3 )) -eq "$val" ]
}

mux_field(){ # addr bit → 当前 2bit 值(读不到则输出 "?")
  local cur
  cur=$(reg_read "$1" 2>/dev/null) || { echo "?"; return 1; }
  echo $(( (cur>>$2) & 3 ))
}

mux_select(){ # 组名 addr bit val → 0=成功。先试 pinmux-select,兜底寄存器直写
  local grp=$1 addr=$2 bit=$3 val=$4
  if [ "$MUX_BACKEND" = pinmux-select ] && [ -n "$LSIO_FUNC" ]; then
    # 内核 pinmux.c 解析顺序 "<组名> <函数名>";保险起见失败再试反序
    if echo "$grp $LSIO_FUNC" > "$LSIO_DBG/pinmux-select" 2>/dev/null \
    || echo "$LSIO_FUNC $grp" > "$LSIO_DBG/pinmux-select" 2>/dev/null; then
      # pinmux-select 写成功不代表 mux 真变了,有寄存器工具就回读硬校验
      if [ -n "$REG_TOOL" ]; then
        [ "$(mux_field "$addr" "$bit")" = "$val" ] && return 0
        log "pinmux-select($grp) 写成功但回读不符,回退寄存器直写"
      else
        return 0
      fi
    else
      log "pinmux-select 失败($grp),回退寄存器直写(工具: ${REG_TOOL:-无})"
    fi
  fi
  reg_rmw "$addr" "$bit" "$val"
}

# ---------- 外设 / 模块 / pps 辅助 ----------
console_is_on(){ # $1 = 平台设备名 → 0=console 就在它上面。纯判定,绝不 exit
  local n devpath
  n=$(sed -n 's/.*console=ttyS\([0-9]\).*/\1/p' /proc/cmdline | head -1)
  [ -z "$n" ] && return 1
  devpath=$(readlink -f "/sys/class/tty/ttyS$n/device" 2>/dev/null) || return 1
  CONSOLE_TTY="ttyS$n"
  case "$devpath" in *"$1"*) return 0;; esac
  return 1
}

console_guard(){ # $1 = 平台设备名;console 在它上面就拦下(--force 放行)
  console_is_on "$1" || return 0
  [ "$FORCE" = 1 ] && { log "警告:console 就在 $1 上,--force 强行继续(准备好 SSH!)"; return 0; }
  die "console=$CONSOLE_TTY 落在 $1 上,解绑会失去控制台。请走 SSH 并加 --force"
}

tty_of(){ ls "/sys/bus/platform/devices/$1/tty" 2>/dev/null | head -1; }

mod_loaded(){ [ -d "/sys/module/$MODNAME" ]; }
mod_param(){ cat "/sys/module/$MODNAME/parameters/$1" 2>/dev/null; }

find_ko(){
  local c
  for c in "${X5PPS_KO:-}" "$(dirname "$(readlink -f "$0")")/x5pps.ko" \
           /userdata/x5-pps/x5pps.ko "/lib/modules/$(uname -r)/extra/x5pps.ko"; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

ko_vermagic(){
  local v=""
  command -v modinfo >/dev/null 2>&1 && v=$(modinfo -F vermagic "$1" 2>/dev/null | head -1)
  [ -z "$v" ] && v=$(tr -c '[:print:]\n' '\n' < "$1" | sed -n 's/^vermagic=//p' | head -1)
  echo "$v"
}

pps_dir(){ # 本模块建的那个 pps(认 name==x5pps,绝不认 hobot-pps.-1)
  local id d
  id=$(mod_param pps_id)
  if [ -n "$id" ] && [ "$id" != "-1" ] && [ -d "/sys/class/pps/pps$id" ]; then
    echo "/sys/class/pps/pps$id"; return 0
  fi
  for d in /sys/class/pps/pps*; do
    [ -e "$d/name" ] || continue
    [ "$(cat "$d/name" 2>/dev/null)" = "$MODNAME" ] && { echo "$d"; return 0; }
  done
  return 1
}

irq_count(){ # 本模块 IRQ 在 /proc/interrupts 的计数总和(多核相加)
  # 只加 CPU 计数列:列数由表头 NF 决定($1 是 "N:",随后 ncpu 列才是计数,
  # 再后面的 chip/hwirq/Level/名字 一律不能加 —— hwirq 是纯数字,漏掉会把它算进去)。
  # 优先按 IRQ 号定位(最准),拿不到号再按名字末列匹配。
  local irqn=${1:-}
  awk -v n="$MODNAME" -v want="$irqn" '
    NR==1 { ncpu=NF; next }
    {
      hit = (want != "" && $1 == want":") || (want == "" && $NF == n)
      if (!hit) next
      s=0; for (i=2; i<=ncpu+1 && i<=NF; i++) if ($i ~ /^[0-9]+$/) s+=$i
      print s; exit
    }' /proc/interrupts 2>/dev/null
}

assert_seq(){ # → "sec nsec seq";读不到则空
  local d v
  d=$(pps_dir) || return 1
  v=$(cat "$d/assert" 2>/dev/null) || return 1
  [ -n "$v" ] || return 1
  echo "$v" | sed 's/[.#]/ /g'
}

# ---------- 状态文件 ----------
state_save(){
  cat > "$STATE" <<EOF
PIN=$P_GPIO
SIG=$P_SIG
GRP=$P_GRP
UGRP=$P_UGRP
ADDR=$P_ADDR
BIT=$P_BIT
UART=$P_UART
DEV=$P_DEV
DRV=$UART_DRV
EDGE=$EDGE
ORIG_REG=${ORIG_REG:-}
UNBOUND=${UNBOUND:-0}
EOF
}
state_load(){ [ -f "$STATE" ] || return 1; . "$STATE"; return 0; }

unbind_uart(){ # $1=平台设备 → 0=真解绑了 1=本来就没绑(可继续) 2=解绑失败(必须中止)
  [ -e "/sys/bus/platform/devices/$1" ] || { log "$1 设备不存在,跳过解绑"; return 1; }
  [ -e "/sys/bus/platform/devices/$1/driver" ] || { log "$1 本就未绑定驱动,跳过解绑"; return 1; }
  echo "$1" > "$UART_DRV/unbind" 2>/dev/null || { log "解绑 $1 失败"; return 2; }
  log "已解绑 $1"
  return 0
}

rebind_uart(){ # $1=平台设备
  [ -e "/sys/bus/platform/devices/$1/driver" ] && { log "$1 已绑定,无需回绑"; return 0; }
  if echo "$1" > "$UART_DRV/bind" 2>/dev/null; then log "已回绑 $1"; return 0; fi
  sleep 1
  echo "$1" > "$UART_DRV/bind" 2>/dev/null && { log "已回绑 $1"; return 0; }
  log "!! $1 回绑失败,请手动: echo $1 > $UART_DRV/bind (或直接重启)"
  return 1
}

# ---------- 持久化 ----------
persist_mode(){ # → pps|uart(无配置=uart,即出厂行为)
  # ★必须在子 shell 里 source:$PERSIST 里有 PIN=/EDGE=,直接 . 会把调用方的
  #   PIN 冲掉(do_uart 早先踩过同一个坑)。
  ( MODE=uart; [ -f "$PERSIST" ] && . "$PERSIST" 2>/dev/null; echo "${MODE:-uart}" )
}

persist_save(){ # $1 = pps|uart
  if [ "$ONCE" = 1 ]; then
    log "--once:只改本次运行,不动持久配置(重启后仍按 $(persist_mode) 模式)"
    return 0
  fi
  mkdir -p "$SELFDIR" 2>/dev/null
  # ★ 下面每个值都必须带引号:STAMP 形如 "Wed Sep  9 11:55:36 CST 2026",不加引号
  #   source 时会被当成 "STAMP=Wed" + 执行命令 Sep,导致 . 返回非零、do_boot 误判
  #   "持久配置读失败",重启后永远回不到 PPS(离线自测抓到)。
  if ! cat > "$PERSIST" <<EOF
# x5-pps 持久模式 —— 由 x5-pps-pin.sh 写入,勿手改。
# 开机时 /etc/init.d/S99auto_startup → $STARTUP → 本脚本 boot 读它生效。
# 切回普通串口: $SELFDIR/${0##*/} uart     彻底移除: $SELFDIR/${0##*/} uninstall
MODE="$1"
PIN="${P_GPIO:-$DEFAULT_PIN}"
EDGE="$EDGE"
KEEP_UART="$KEEP_UART"
FORCE="$FORCE"
STAMP="$(date 2>/dev/null || echo unknown)"
EOF
  then
    log "!! 写不了 $PERSIST(目录只读?),本次切换重启后会丢失"
    return 1
  fi
  hook_install || return 1
  if [ "$1" = pps ]; then
    log "持久配置已写:此后每次重启都自动进 PPS 输入(GPIO${P_GPIO:-?} edge=$EDGE),直到你再跑一次 '${0##*/} uart'"
  else
    log "持久配置已写:此后每次重启都保持普通串口功能,直到你再跑一次 '${0##*/} pps'"
  fi
}

hook_install(){ # 幂等地把调用行写进 /userdata/startup.sh;绝不覆盖别人已有的内容
  local self="$SELFDIR/${0##*/}"
  local sdir; sdir=$(dirname "$STARTUP")
  [ -d "$sdir" ] || { log "!! 没有 $sdir,开机钩子装不了(持久配置已写,但重启不会自动生效)"; return 1; }
  if [ -f "$STARTUP" ] && grep -qF "$HOOK_BEGIN" "$STARTUP" 2>/dev/null; then
    chmod +x "$STARTUP" 2>/dev/null
    log "开机钩子已在 $STARTUP 里,无需重复安装"
    return 0
  fi
  if [ ! -f "$STARTUP" ]; then
    printf '#!/bin/sh\n# 由 x5-pps-pin.sh 创建。本文件由 /etc/init.d/S99auto_startup 在开机末尾执行。\n\n' \
      > "$STARTUP" || { log "!! 创建 $STARTUP 失败"; return 1; }
  fi
  # 只追加,不动原有任何一行(板上可能已经有 CAN 黑匣子之类在用这个文件)
  {
    echo "$HOOK_BEGIN"
    echo "[ -x $self ] && $self boot >> $BOOTLOG 2>&1 &"
    echo "$HOOK_END"
  } >> "$STARTUP" || { log "!! 往 $STARTUP 追加失败"; return 1; }
  chmod +x "$STARTUP" 2>/dev/null
  log "开机钩子已装进 $STARTUP(原有内容一行未动)"
}

hook_uninstall(){
  [ -f "$STARTUP" ] || { log "$STARTUP 不存在,无钩子可删"; return 0; }
  grep -qF "$HOOK_BEGIN" "$STARTUP" 2>/dev/null || { log "$STARTUP 里没有本脚本的钩子"; return 0; }
  local tmp="$STARTUP.x5pps.tmp"
  awk -v b="$HOOK_BEGIN" -v e="$HOOK_END" '
    index($0,b){skip=1; next}
    index($0,e){skip=0; next}
    !skip' "$STARTUP" > "$tmp" 2>/dev/null || { log "!! 改写 $STARTUP 失败"; rm -f "$tmp"; return 1; }
  cat "$tmp" > "$STARTUP" || { log "!! 写回 $STARTUP 失败"; rm -f "$tmp"; return 1; }
  rm -f "$tmp"
  log "已从 $STARTUP 摘掉开机钩子(其余内容原样保留)"
}

hook_status(){
  log "--- 持久化(重启后行为) ---"
  [ -e "$DISABLE" ] && echo "  ★ 救命开关 $DISABLE 存在 ⇒ 开机钩子一律不动手"
  if [ "$(persist_mode)" = pps ]; then
    echo "  持久模式: PPS 输入   $(grep -hE '^(PIN|EDGE|FORCE|STAMP)=' "$PERSIST" 2>/dev/null | tr '\n' ' ')"
  elif [ -f "$PERSIST" ]; then
    echo "  持久模式: 普通串口(显式写过)"
  else
    echo "  持久模式: 普通串口(无 $PERSIST,即出厂行为)"
  fi
  if [ -f "$STARTUP" ] && grep -qF "$HOOK_BEGIN" "$STARTUP" 2>/dev/null; then
    if [ -x "$STARTUP" ]; then echo "  开机钩子: 已装 ($STARTUP)"
    else echo "  开机钩子: 已装但 $STARTUP 不可执行 ⇒ ★S99auto_startup 不会跑它!  chmod +x $STARTUP"; fi
  else
    echo "  开机钩子: 未装 ⇒ 重启不会自动切换"
  fi
  [ -s "$BOOTLOG" ] && echo "  上次开机日志: $BOOTLOG  (tail -20 $BOOTLOG)"
  return 0
}

# ================= 各子命令 =================

do_check(){
  detect_mux_backend
  echo
  log "可选引脚(MUX_ALT0=串口功能, MUX_ALT2=GPIO/PPS):"
  printf '  %-5s %-7s %-15s %-14s %-11s %-4s %-6s %-16s %s\n' \
    全局号 排针 信号 gpio组 mux寄存器 bit 属主 平台设备 当前mux
  local r cur mark
  for r in "${PINS[@]}"; do
    IFS='|' read -r g hdr sig grp addr bit alt ua ugrp dev note <<<"$r"
    cur=$(mux_field "$addr" "$bit")
    case "$cur" in
      0) mark="ALT0=串口";;
      2) mark="ALT2=GPIO";;
      '?') mark="读不到";;
      *) mark="ALT$cur=?";;
    esac
    printf '  %-5s %-7s %-15s %-14s %-11s %-4s %-6s %-16s %s\n' \
      "$g" "$hdr" "$sig" "$grp" "$addr" "$bit" "$ua" "$dev" "$mark"
    [ -n "$note" ] && printf '        %s\n' "$note"
  done
  echo
  log "默认引脚: $DEFAULT_PIN"
  local ko
  if ko=$(find_ko); then
    log "找到模块: $ko  vermagic=[$(ko_vermagic "$ko")]"
    log "板上内核: [$(uname -r)]"
    [ "$(ko_vermagic "$ko" | awk '{print $1}')" = "$(uname -r)" ] \
      && log "vermagic 匹配 OK" || log "!! vermagic 不匹配,insmod 会被拒"
  else
    log "!! 没找到 x5pps.ko(找过 \$X5PPS_KO / 脚本同目录 / /userdata/x5-pps/ / /lib/modules/\$(uname -r)/extra/)"
  fi
  echo
  log "当前 /sys/class/pps/*:"
  local d nm
  for d in /sys/class/pps/pps*; do
    [ -e "$d/name" ] || continue
    nm=$(cat "$d/name" 2>/dev/null)
    if [ "$nm" = "$MODNAME" ]; then
      printf '  %s  name=%-14s path=%-10s  ← 本脚本建的真源\n' "${d##*/}" "$nm" "$(cat "$d/path" 2>/dev/null)"
    else
      printf '  %s  name=%-14s path=%-10s  ← ★假源(hobot-pps 是 1 秒 mod_timer 伪造,跟引脚无关)\n' \
        "${d##*/}" "$nm" "$(cat "$d/path" 2>/dev/null)"
    fi
  done
  [ -e /sys/class/pps/pps0 ] || log "  (无)"
  echo
  log "--check 结束,未做任何修改"
}

do_pps(){
  local ko vm
  ko=$(find_ko) || die "找不到 x5pps.ko;放到脚本同目录,或 export X5PPS_KO=/path/x5pps.ko"
  vm=$(ko_vermagic "$ko" | awk '{print $1}')
  if [ "$vm" != "$(uname -r)" ]; then
    [ "$FORCE" = 1 ] || die "vermagic 不匹配:ko=[$vm] 板上=[$(uname -r)];用对内核重编(make KDIR=...),或 --force 硬试"
    log "警告:vermagic 不匹配($vm vs $(uname -r)),--force 硬试"
  fi

  if mod_loaded; then
    local cur_gpio; cur_gpio=$(mod_param gpio)
    if [ "$cur_gpio" = "$P_GPIO" ]; then
      log "已经是 PPS 模式(gpio=$cur_gpio),无需重复切换"; do_status; return 0
    fi
    die "模块已加载在 gpio=$cur_gpio 上。先 '$0 uart' 还原,再切到 $P_GPIO"
  fi

  if [ "$KEEP_UART" = 1 ]; then
    # --keep-uart 不解绑,console 的"设备绑定"不会丢,所以不硬拦;但 pad 还是被借走了 ——
    # 借的是 UART7 RX 脚(379),console 输入会受到影响,且没有任何报错,必须提醒。
    console_is_on "$P_DEV" && \
      log "警告:console($CONSOLE_TTY)就在 $P_UART 上。--keep-uart 不解绑,但 UART7 RX 和 TX IO mux 都会改变,console 可能失去输入/输出且无额外报错"
  else
    console_guard "$P_DEV"
  fi

  ORIG_REG=""
  if [ -n "$REG_TOOL" ]; then
    local v; v=$(reg_read "$P_ADDR") && ORIG_REG=$(printf '0x%08x' "$v")
    log "保存 $P_ADDR 原值 = ${ORIG_REG:-读失败}"
  fi

  UNBOUND=0
  # 从这里开始出错就要回滚
  rollback(){
    log "出错,回滚中..."
    rmmod "$MODNAME" 2>/dev/null
    if [ -n "$ORIG_REG" ] && [ -n "$REG_TOOL" ]; then
      reg_write "$P_ADDR" "$((ORIG_REG))" 2>/dev/null
    else
      mux_select "$P_UGRP" "$P_ADDR" "$P_BIT" 0 2>/dev/null || true
      mux_select "$IO_GRP" "$IO_ADDR" "$IO_BIT" 0 2>/dev/null || true
    fi
    [ "$UNBOUND" = 1 ] && rebind_uart "$P_DEV"
    rm -f "$STATE"
  }
  trap 'rollback' EXIT INT TERM

  if [ "$KEEP_UART" = 1 ]; then
    log "--keep-uart:不解绑 $P_UART,只翻 mux(注意 uart 若走 PM suspend/resume 会静默把 pad 抢回去)"
  else
    unbind_uart "$P_DEV"; case $? in
      0) UNBOUND=1;;
      2) die "解绑 $P_DEV 失败,没敢继续翻 mux(pad 仍是串口功能,现场未改动)";;
      *) :;;   # 本来就没绑,继续
    esac
  fi
  state_save

  mux_select "$P_GRP" "$P_ADDR" "$P_BIT" "$P_ALT" \
    || die "mux 切 ALT$P_ALT 失败,pad 仍在串口功能"
  if [ -n "$REG_TOOL" ]; then
    local now; now=$(mux_field "$P_ADDR" "$P_BIT")
    [ "$now" = "$P_ALT" ] || die "mux 回读 = ALT$now,不是 ALT$P_ALT,切换没生效"
    log "mux 已切: $P_ADDR bit$P_BIT = ALT$P_ALT (GPIO),回读校验通过"
  else
    log "mux 已写 ALT$P_ALT(无寄存器工具,未能回读校验)"
  fi

  mux_select "$IO_GRP" "$IO_ADDR" "$IO_BIT" "$IO_ALT" \
    || die "UART7 TX mux 切 GPIO/IO 失败"
  if [ -n "$REG_TOOL" ]; then
    local io_now; io_now=$(mux_field "$IO_ADDR" "$IO_BIT")
    [ "$io_now" = "$IO_ALT" ] || die "UART7 TX mux 回读 = ALT$io_now,不是 ALT$IO_ALT"
    log "UART7 TX 已释放为 GPIO/IO: GPIO$IO_GPIO ($IO_HDR $IO_SIG),回读校验通过"
  else
    log "UART7 TX 已尝试切为 GPIO/IO(无寄存器工具,未能回读校验)"
  fi

  insmod "$ko" gpio="$P_GPIO" pps_id=2 edge="$EDGE" \
    || die "insmod 失败;dmesg 尾部看原因(常见:gpio 被别的驱动占了 → cat /sys/kernel/debug/gpio)"

  trap - EXIT INT TERM
  state_save
  log "已切到 PPS 输入: GPIO$P_GPIO ($P_HDR $P_SIG) edge=$EDGE"
  persist_save pps
  do_status
  echo
  log "验真必须做: $0 test --secs 10   —— 中断计数不涨就是没通,前面全白搭"
  log "还原:       $0 uart   (同时把持久配置改回串口,重启也不再进 PPS)"
}

do_uart(){
  # 认引脚的优先级:① 模块自己记的 gpio(最可靠) ② 状态文件 ③ 命令行 --pin
  # 注意 state_load 是 ". $STATE",里面有 PIN= 会把命令行的 $PIN 覆盖掉,
  # 所以先把命令行值存下来,状态文件只在前两级都拿不到时才用。
  local cli_pin="${PIN:-}" g=""

  if mod_loaded; then
    g=$(mod_param gpio)
    log "卸载 $MODNAME (gpio=${g:-?}, assert=$(mod_param assert_count) clear=$(mod_param clear_count))"
    rmmod "$MODNAME" || die "rmmod 失败(有进程占着 /dev/ppsN? fuser -v /dev/pps*)"
    [ -n "$g" ] && pin_lookup "$g" || log "模块 gpio 参数异常(${g:-空}),改用状态文件/--pin"
  fi
  if [ -z "$ROW" ] && state_load; then
    # ORIG_REG/UNBOUND 等兜底信息也一并从状态文件带出来
    [ -n "${PIN:-}" ] && pin_lookup "$PIN" || true
  fi
  [ -z "$ROW" ] && [ -n "$cli_pin" ] && { pin_lookup "$cli_pin" || die "--pin $cli_pin 不在表里"; }
  [ -n "$ROW" ] || die "认不出要还原哪个脚:模块没加载、状态文件($STATE)也没有。请指定 --pin 379"

  # 2) 回绑 uart —— probe 时 pinctrl 会把 "default" 状态写回,mux 自动回 ALT0
  if [ -e "/sys/bus/platform/devices/$P_DEV" ]; then rebind_uart "$P_DEV"; fi

  # 3) 回读校验;没回到 ALT0 就自己写回
  if [ -n "$REG_TOOL" ]; then
    local now; now=$(mux_field "$P_ADDR" "$P_BIT")
    if [ "$now" != "0" ]; then
      log "回绑后 mux 仍是 ALT$now,直接写回 ALT0..."
      if [ -n "${ORIG_REG:-}" ]; then
        reg_write "$P_ADDR" "$((ORIG_REG))" && log "已按状态文件原值 $ORIG_REG 恢复整寄存器"
      fi
      now=$(mux_field "$P_ADDR" "$P_BIT")
      [ "$now" = "0" ] || mux_select "$P_UGRP" "$P_ADDR" "$P_BIT" 0 || true
      now=$(mux_field "$P_ADDR" "$P_BIT")
    fi
    [ "$now" = "0" ] && log "mux 回读 = ALT0(串口功能),校验通过" \
                     || log "!! mux 回读 = ALT$now,没能还原;直接 reboot 即完全恢复"
  else
    mux_select "$P_UGRP" "$P_ADDR" "$P_BIT" 0 || true
    mux_select "$IO_GRP" "$IO_ADDR" "$IO_BIT" 0 || true
    log "已尝试写回 UART7 RX/TX 的 ALT0(无寄存器工具,未能回读校验)"
  fi

  if [ -n "$REG_TOOL" ]; then
    local io_now; io_now=$(mux_field "$IO_ADDR" "$IO_BIT")
    [ "$io_now" = "0" ] && log "UART7 TX mux 回读 = ALT0(串口功能),校验通过" \
                        || log "!! UART7 TX mux 回读 = ALT$io_now,未能还原;直接 reboot 即完全恢复"
  fi

  local t; t=$(tty_of "$P_DEV")
  [ -n "$t" ] && log "串口已回来: /dev/$t ($P_UART $P_DEV)" \
              || log "!! $P_DEV 下没看到 tty,串口没起来;直接 reboot 即完全恢复"
  rm -f "$STATE"
  log "已还原为普通串口功能: GPIO$P_GPIO ($P_HDR $P_SIG)"
  persist_save uart
}

do_status(){
  local d nm id
  echo
  log "--- 模块 ---"
  if mod_loaded; then
    printf '  %s 已加载: gpio=%s edge=%s irq=%s pps_id=%s assert=%s clear=%s\n' \
      "$MODNAME" "$(mod_param gpio)" "$(mod_param edge)" "$(mod_param irq)" \
      "$(mod_param pps_id)" "$(mod_param assert_count)" "$(mod_param clear_count)"
  else
    echo "  $MODNAME 未加载(当前不是 PPS 模式)"
  fi

  log "--- 引脚 mux ---"
  local r cur mark
  for r in "${PINS[@]}"; do
    IFS='|' read -r g hdr sig grp addr bit alt ua ugrp dev note <<<"$r"
    cur=$(mux_field "$addr" "$bit")
    case "$cur" in 0) mark="ALT0 串口功能";; 2) mark="ALT2 GPIO/PPS";; \?) mark="读不到";; *) mark="ALT$cur ?";; esac
    printf '  GPIO%-4s %-7s %-15s %-16s %s%s\n' "$g" "$hdr" "$sig" "$mark" \
      "$(tty_of "$dev" | sed 's|^|tty=/dev/|')" \
      "$([ -e "/sys/bus/platform/devices/$dev/driver" ] && echo "" || echo "  [已解绑]")"
  done

  if [ -n "$REG_TOOL" ]; then
    printf "  UART7 TX IO mux: GPIO%s %s = ALT%s (%s)\n" "$IO_GPIO" "$IO_SIG" "$(mux_field "$IO_ADDR" "$IO_BIT")" "$IO_HDR"
  fi

  log "--- /sys/class/pps ---"
  for d in /sys/class/pps/pps*; do
    [ -e "$d/name" ] || continue
    nm=$(cat "$d/name" 2>/dev/null)
    if [ "$nm" = "$MODNAME" ]; then
      printf '  %-6s name=%-14s path=%-10s mode=%s  ← 真源(本脚本)\n' \
        "${d##*/}" "$nm" "$(cat "$d/path" 2>/dev/null)" "$(cat "$d/mode" 2>/dev/null)"
      printf '         assert=%s\n' "$(cat "$d/assert" 2>/dev/null)"
    else
      printf '  %-6s name=%-14s ← ★假源:hobot-pps 是 1 秒 mod_timer 伪造的,跟引脚无关,别拿它验收\n' \
        "${d##*/}" "$nm"
    fi
  done

  log "--- /proc/interrupts ---"
  grep -w "$MODNAME" /proc/interrupts 2>/dev/null || echo "  (没有 $MODNAME 的中断行)"

  hook_status
}

do_test(){
  mod_loaded || die "模块没加载,先跑 '$0 pps'"
  local d; d=$(pps_dir) || die "找不到 name==$MODNAME 的 pps 设备(别认 hobot-pps.-1 那个假的)"
  local irqn; irqn=$(mod_param irq)
  # 按模块记的 gpio 补齐 P_*,否则 set -u 下 FAIL 分支引用 $P_HDR 会当场 unbound variable
  [ -z "$ROW" ] && pin_lookup "$(mod_param gpio)" 2>/dev/null || true

  local i1 a1 s1 n1 q1 i2 a2 s2 n2 q2
  i1=$(irq_count "$irqn"); a1=$(mod_param assert_count); a1=${a1:-0}
  read -r s1 n1 q1 <<<"$(assert_seq)"
  log "采样 ${SECS}s ... (gpio=$(mod_param gpio) irq=$irqn edge=$(mod_param edge))"
  log "  起点: irq计数=${i1:-?} assert_count=$a1 seq=${q1:-?}"
  sleep "$SECS"
  i2=$(irq_count "$irqn"); a2=$(mod_param assert_count); a2=${a2:-0}
  read -r s2 n2 q2 <<<"$(assert_seq)"
  log "  终点: irq计数=${i2:-?} assert_count=$a2 seq=${q2:-?}"

  local d_irq="?" d_a d_q="?"
  [ -n "${i1:-}" ] && [ -n "${i2:-}" ] && d_irq=$((i2-i1))
  d_a=$((a2-a1))
  [ -n "${q1:-}" ] && [ -n "${q2:-}" ] && d_q=$((q2-q1))
  echo
  log "${SECS}s 内:中断 +$d_irq,assert 事件 +$d_a,PPS 序号 +$d_q"

  # 平均周期:整数纳秒算,避免 awk 双精度吃不下 epoch+ns;10# 强制十进制(nsec 有前导零)
  if [ -n "${s1:-}" ] && [ -n "${s2:-}" ] && [ "${d_q:-0}" != "?" ] && [ "${d_q:-0}" -gt 0 ]; then
    local dns per
    dns=$(( (10#$s2 - 10#$s1) * 1000000000 + (10#$n2 - 10#$n1) ))
    per=$(( dns / d_q ))
    printf '%s 平均周期 = %d.%06d s (期望 1.000000)\n' "$LOGTAG" $((per/1000000000)) $(( (per%1000000000)/1000 ))
  fi

  echo
  if [ "$d_a" -eq 0 ]; then
    log "结果: FAIL —— 一个事件都没有。查:①信号线真的接到 ${P_HDR:-该脚} 了吗 ②共地了吗"
    log "        ③mux 是不是 ALT2($0 status) ④边沿方向对不对(--edge falling 试试)"
    log "        ⑤PPS 源是 3.3V 电平吗(pad 是 3.3V 域,1.8V 输出要加电平转换)"
    return 1
  fi
  local lo=$((SECS-2)) hi=$((SECS+2))
  [ "$lo" -lt 1 ] && lo=1
  if [ "$d_a" -ge "$lo" ] && [ "$d_a" -le "$hi" ]; then
    log "结果: PASS —— ${SECS}s 收到 $d_a 个事件,约 1 Hz,真 PPS 输入已通"
    log "反证建议:拔掉信号线再跑一次,必须变成 0 —— 这才排除了 hobot-pps 那个假源"
    return 0
  fi
  log "结果: 有信号但速率不是 1 Hz(${SECS}s 收到 $d_a 个)。抖动/双沿/信号源频率不对?"
  log "        edge=both 会双倍计数;确认信号源确实是 1PPS"
  return 1
}

do_boot(){ # 开机钩子专用。铁律:无论如何都 return 0,绝不能挡住开机
  echo "===== $(date 2>/dev/null || echo '(无 RTC)') x5-pps boot hook ====="
  [ -e "$DISABLE" ] && { log "救命开关 $DISABLE 存在,开机不做任何切换"; return 0; }
  [ -f "$PERSIST" ] || { log "无持久配置,保持出厂普通串口功能"; return 0; }
  # 这里是有意直接 source 到全局:PIN/EDGE/KEEP_UART/FORCE 正是本次要用的参数
  . "$PERSIST" 2>/dev/null || { log "持久配置读失败,保持出厂普通串口功能"; return 0; }
  [ "${MODE:-uart}" = pps ] || { log "持久模式=${MODE:-uart},保持普通串口功能,不动手"; return 0; }

  EDGE=${EDGE:-rising}; KEEP_UART=${KEEP_UART:-0}; FORCE=${FORCE:-0}
  ONCE=1   # 开机路径绝不回写持久配置,免得把用户的配置改花
  pin_lookup "${PIN:-$DEFAULT_PIN}" || { log "!! 持久配置里 PIN=${PIN:-空} 不在表里,放弃切换"; return 0; }

  # 开机时没人在旁边,console 一旦丢就只能拆机接串口救。除非当初存配置时明确 --force 过。
  if console_is_on "$P_DEV" && [ "$FORCE" != 1 ]; then
    log "!! console($CONSOLE_TTY)落在 $P_UART 上,开机切换会丢控制台,已跳过(当初用 --force 存的配置才照做)"
    return 0
  fi
  log "按持久配置切 PPS: GPIO$P_GPIO ($P_HDR $P_SIG) edge=$EDGE"
  ( do_pps ) || log "!! 开机切换失败(原因见上),已保持串口功能;开机流程不受影响"
  return 0
}

do_install(){
  [ "$(persist_mode)" = pps ] \
    || log "注意:当前持久模式是 uart(开机不切)。要让重启后自动进 PPS,直接跑 '${0##*/} pps'"
  hook_install
  hook_status
}

do_uninstall(){
  hook_uninstall
  [ -f "$PERSIST" ] && { rm -f "$PERSIST" && log "已删除持久配置 $PERSIST"; }
  log "开机起不再做任何切换。当前运行时状态未变 —— 要立刻回普通串口请跑 '${0##*/} uart --once'"
}

# ================= 主流程 =================
[ "$(id -u)" = 0 ] || die "需要 root"

# check/install/uninstall 是探测与恢复路径:环境再差(没 debugfs、没 devmem)也必须能跑,
# 所以放在 detect_mux_backend 之前 —— 后者探不到后端会直接 die。
case "$CMD" in
  check)     do_check;      exit 0;;
  install)   do_install;    exit 0;;
  uninstall) do_uninstall;  exit 0;;
esac

detect_mux_backend

case "$CMD" in
  pps)
    pin_lookup "${PIN:-$DEFAULT_PIN}" || die "不认识的引脚 '${PIN:-$DEFAULT_PIN}';可选: $(pin_list | tr '\n' ' ')"
    case "$EDGE" in rising|falling|both) ;; *) die "--edge 只能是 rising|falling|both";; esac
    log "目标: GPIO$P_GPIO ($P_HDR $P_SIG) 属主=$P_UART($P_DEV) edge=$EDGE"
    [ -n "$P_NOTE" ] && log "注意: $P_NOTE"
    do_pps;;
  uart)
    [ -n "$PIN" ] && { pin_lookup "$PIN" || die "不认识的引脚 '$PIN'"; }
    do_uart;;
  status)
    [ -n "$PIN" ] && pin_lookup "$PIN"
    do_status;;
  test)
    if mod_loaded; then pin_lookup "$(mod_param gpio)" 2>/dev/null || true; fi
    do_test;;
  boot)      do_boot;;
  *) die "未知命令 $CMD";;
esac
