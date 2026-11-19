#!/bin/sh
# SC132 单目启动：在 X5 上复用运行包 bin/cam_demo，前台运行相机和 RTSP。
# 运行包须支持 --camera-id 和 SC132_SENSOR_PROFILE；无需重新构建或替换库。
#
# 用法（在 Demo 仓库根目录执行）：
#   sh scripts/single_camera.sh 3                  # 相机3，默认 master
#   sh scripts/single_camera.sh 3 --mode slave     # 同一颗相机切换为 slave
#   sh scripts/single_camera.sh 3 --dry-run        # 只显示配置，不访问硬件
#   sh scripts/single_camera.sh --help            # 查看全部参数和功能
#
# 参数：
#   <camera-id>                 必填物理相机编号：0、1、2、3，不是相机数量。
#   --mode <master|slave>       sensor 模式，默认 master。
#   --fps <25|30|40|50|60>       相机 FPS 和 GPIO 触发频率，默认 30。
#   --demo-dir <绝对路径>       运行包目录，默认 ${DEMO_DIR:-/root/demo}。
#   --rtsp-base-port <端口>     RTSP 基准端口，默认 1554；范围 1..65532，
#                              十进制且无前导零，实际端口为基准端口 + 相机ID。
#   --dry-run                  仅显示 profile、环境配置、RTSP 地址和启动参数。
#   -h, --help                 显示帮助，不要求相机ID，也不访问硬件。
#
# RTSP 地址为 rtsp://<板卡IP>:<基准端口+相机ID>/PRR，相机3默认端口为1557。
# 脚本启用 sensor probe 和 diagnostics，并在当前进程覆盖 SC132_SENSOR_PROFILE、
# SC132_TRIGGER_MODE=software_gpio、SC132_TRIGGER_HZ=<fps>、SC132_SENSOR_PROBE=1。
# master 的30fps使用 SDK base profile 52，其他FPS使用5；slave 的25/30fps使用53，
# 其他FPS使用63。实际FPS由 VTS 配置决定，不等于 base profile 名称中的帧率。
# master/slave 只切换 sensor profile；当前库在 master 下也输出 GPIO417 脉冲，
# 不是关闭 GPIO 的对照实验。运行前确保没有其他应用占用相机。
# Ctrl+C 完整退出后再切换模式；exec 保留 consumer 的退出码和终端信号。
# 脚本不自动重试、不停止服务或其他进程，不绕过 SDK HS 检查；单次出流不代表可靠性验收。
set -eu

usage() {
  cat <<'USAGE'
用法：sh single_camera.sh <camera-id: 0|1|2|3> [选项]

功能：在 X5 上前台运行一颗物理 SC132 相机和 RTSP，支持 master/slave 切换。
只需输入相机ID，默认 master、30fps；按 Ctrl+C 退出。

参数与选项：
  <camera-id>                 必填物理相机编号：0、1、2、3（不是相机数量）
  --mode <master|slave>       sensor 模式，默认 master
  --fps <25|30|40|50|60>       相机 FPS 和 GPIO 触发频率，默认 30
  --demo-dir <绝对路径>       运行包目录，默认 ${DEMO_DIR:-/root/demo}
  --rtsp-base-port <端口>     RTSP 基准端口，默认 1554；范围 1..65532
                              十进制且无前导零，实际端口 = 基准端口 + 相机ID
  --dry-run                  仅显示配置、RTSP 地址和启动参数，不访问硬件
  -h, --help                 显示本帮助，无需相机ID，不访问硬件

示例（在 Demo 仓库根目录执行）：
  sh scripts/single_camera.sh 3
  sh scripts/single_camera.sh 3 --mode slave
  sh scripts/single_camera.sh 3 --fps 60
  sh scripts/single_camera.sh 3 --demo-dir /absolute/package/path
  sh scripts/single_camera.sh 3 --dry-run

RTSP：rtsp://<板卡IP>:<基准端口+相机ID>/PRR，相机3默认端口为1557。
自动启用 sensor probe 和 diagnostics；profile、触发模式/频率和 probe 设置
覆盖继承环境，仅影响当前进程。运行包须支持 --camera-id 和 profile 覆盖。
master/slave 只切换 sensor profile，当前库在 master 下也输出 GPIO417 脉冲。
运行前确保相机未被其他应用占用；切换模式前先按 Ctrl+C 完整退出。
脚本不自动重试、不停止服务或其他进程，原样保留 cam_demo 退出码。
USAGE
}

fail() {
  printf 'single_camera: %s\n' "$*" >&2
  exit 2
}

camera_id=
mode=master
fps=30
demo_dir=${DEMO_DIR:-/root/demo}
base_port=1554
dry_run=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --mode|--fps|--demo-dir|--rtsp-base-port)
      [ "$#" -ge 2 ] || fail "$1 requires a value"
      case "$1" in
        --mode) mode=$2 ;;
        --fps) fps=$2 ;;
        --demo-dir) demo_dir=$2 ;;
        --rtsp-base-port) base_port=$2 ;;
      esac
      shift 2
      ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    0|1|2|3)
      [ -z "$camera_id" ] || fail 'provide only one camera id'
      camera_id=$1
      shift
      ;;
    *) fail "unknown argument: $1" ;;
  esac
done

case "$camera_id" in 0|1|2|3) ;; *) fail 'camera id must be 0, 1, 2, or 3' ;; esac
case "$mode" in master|slave) ;; *) fail '--mode must be master or slave' ;; esac
case "$fps" in 25|30|40|50|60) ;; *) fail '--fps must be 25, 30, 40, 50, or 60' ;; esac
case "$demo_dir" in /*) ;; *) fail '--demo-dir must be an absolute path' ;; esac
case "$base_port" in ''|0*|*[!0-9]*) fail '--rtsp-base-port must be a decimal integer in 1..65532 (no leading zeros)' ;; esac
[ "${#base_port}" -le 5 ] && [ "$base_port" -le 65532 ] || fail '--rtsp-base-port must be in 1..65532'

# Match the existing FPS base-profile policy, then explicitly override the mode.
case "$mode:$fps" in
  master:30) profile=sc132gs_linear_1088x1280_raw10_30fps_1lane; index=52 ;;
  master:*) profile=sc132gs_linear_1088x1280_raw10_60fps_1lane; index=5 ;;
  slave:25|slave:30) profile=sc132gs_linear_1088x1280_raw10_30fps_slave_1lane_right; index=53 ;;
  slave:*) profile=sc132gs_linear_1088x1280_raw10_60fps_1lane_slave; index=63 ;;
esac
export SC132_SENSOR_PROFILE="$profile"
export SC132_TRIGGER_MODE=software_gpio SC132_TRIGGER_HZ="$fps" SC132_SENSOR_PROBE=1
export LD_LIBRARY_PATH="${demo_dir}/lib:/usr/hobot/lib:/usr/hobot/lib/sensor:/usr/lib:/lib64:/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
set -- "${demo_dir}/bin/cam_demo" --camera-id "$camera_id" --fps "$fps"
set -- "$@" --trigger-mode software_gpio --rtsp-base-port "$base_port" --diagnostics

printf 'camera_id=%s mode=%s fps=%s\n' "$camera_id" "$mode" "$fps"
printf 'SC132_SENSOR_PROFILE=%s (index=%s)\n' "$SC132_SENSOR_PROFILE" "$index"
printf 'SC132_TRIGGER_MODE=%s SC132_TRIGGER_HZ=%s SC132_SENSOR_PROBE=%s\n' \
  "$SC132_TRIGGER_MODE" "$SC132_TRIGGER_HZ" "$SC132_SENSOR_PROBE"
printf 'RTSP URL: rtsp://<board-ip>:%s/PRR\n' "$((base_port + camera_id))"
printf 'cam_demo arguments (one per line):\n'
printf '  %s\n' "$@"
[ "$dry_run" -eq 0 ] || exit 0
[ -x "$1" ] || fail "cam_demo is missing or not executable: $1"
# exec preserves the consumer's status and delivers terminal signals directly to it.
exec "$@"
