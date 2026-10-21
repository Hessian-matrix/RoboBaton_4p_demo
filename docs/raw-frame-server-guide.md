# 时间戳寻址 raw 帧服务

`sensor_demo` 可按需提供一个 TCP 服务,返回与请求时间戳配对的紧凑 NV12 原帧。标定等消费方用它取回未经 H.264 量化的灰度原图,不必依赖 RTSP 解码路径。

## 1. 用途与边界

服务在 pipeline worker 的 `on_queued_frame` 钩子中只 retain 一次、把帧元数据入队;私有 writer 线程拷贝紧凑 NV12 像素进每路 ring 后 release,不阻塞帧路径。accept 线程独立应答单请求/单响应连接。

- 按需(on-demand)服务:不做全量旁路,连续多路原帧带宽不可行,只服务请求到达时 ring 内已有的帧。
- 时间戳是帧身份:`group_timestamp_ns` 与 RTSP RTP 时间戳同源(仅 ns→μs→90kHz 量化差 ≤11μs)。
- ring 每路定深(默认 16,上限 64),只保留最近若干帧;请求过老的帧返回 no-match。

## 2. 启用

```bash
DEMO_DIR=/root/demo
cd ${DEMO_DIR}
. ./env.sh
./sensor_demo \
  --raw-server-port 4200 \
  --raw-server-ring-depth 16
```

参数:

```text
--raw-server-port <1..65535>      启用服务并指定监听端口;默认 0(关闭)
--raw-server-ring-depth <1..64>   每路 ring 深度,默认 16
```

YAML:

```yaml
raw_server:
  enable: true
  port: 4200
  ring_depth: 16
```

## 3. 协议

TCP 单请求/单响应,请求行以换行结尾:

```text
GET <camera_id> <group_timestamp_ns>
LATEST <camera_id>
```

`camera_id` 为 `0..3`;`group_timestamp_ns` 为十进制无符号整数。

响应为 96 字节小端 packed 头(`RawFrameResponseHeader`)后接紧凑 NV12 payload(仅 status==0 时):

```text
magic              u32   "RWF1" = 0x31574652
version            u32   1
status             i32   0 ok / 1 no-match / 2 bad-request / 3 camera-disabled / 4 no-frame
camera_id          i32
group_timestamp_ns u64
camera_timestamp_ns u64
frame_id           u64
group_id           u64
width              u32
height             u32
stride             u32
vstride            u32
payload_size       u64
reserved           u64[3]
```

payload 布局与 `--capture-dir` 写盘的 `.nv12` 一致:Y 逐行按 stride 截取 width,UV 交错同样逐行截取 width,总长 `width*height*3/2`。

匹配规则:在 ring 内找 `group_timestamp_ns` 与请求值距离最小且 ≤ 半帧容差的帧;无匹配返回 no-match。`LATEST` 返回 sequence 最大的一帧。

## 4. 验收

```text
SENSOR_RAW_SERVER_RESULT port=4200 raw_frames_retained=N raw_frames_copied=N ...
```

`raw_frames_retained` 与 `raw_frames_copied` 相等且 `raw_retain_failures=0`,说明 writer 归还了全部租约;进程退出码非 0 时服务失败详情由 `fatal` 行给出。

## 5. 注意

- 单连接读写硬超时 5s,慢客户端只延迟自身请求,不占用 accept 线程。
- 服务关闭后 ring 不再接受新帧,已建立连接按 in-flight 串行应答完即断开。
- 只服务相机 mask 内的通道;对禁用通道的请求返回 camera-disabled。
