# 同进程 tee 采集测试说明

本页是 **内部测试与取证说明**,不是产品功能文档。tee 采集用于离线可视化、编码质量对照与原始数据取证,不进入产品交付承诺。

## 1. 用途与边界

`--capture-dir` 在同一进程内、RTSP 发送之前,把每路图像的紧凑 NV12 平面和编码器 access unit 落盘。它不改变 RTSP 输出,可与 ROS1 bag / MP4 保存并存。

- 非发布功能;数据只用于开发验证与问题定位。
- 原始帧按帧序完整保留,编码帧按 AU 与 raw 同序号一一对应。

## 2. 采集

```bash
DEMO_DIR=/root/test/demo   # 实际运行包目录
cd ${DEMO_DIR}
. ./env.sh
./sensor_demo \
  --fps 30 \
  --capture-dir /data/capture_run1 \
  --capture-frame-count 100 \
  --capture-warmup-seconds 5
```

参数:

```text
--capture-dir <absolute-directory>   启用采集并指定输出目录(必须绝对路径)
--capture-frame-count <1..100>       每路目标帧数,默认 100
--capture-warmup-seconds <1..60>     相机启动后等待秒数,默认 5
```

YAML:

```yaml
capture:
  save: true
  save_path: /data/capture_run1
  frame_count: 100
  warmup_seconds: 5
```

输出布局:

```text
<capture-dir>/
  camN/raw/NNNNN.nv12      紧凑 NV12(Y 逐行按 stride 截取 width,UV 交错同样逐行截取)
  camN/au/NNNNN.<codec>    Annex-B access unit;扩展名随 --codec:h264 / h265
  camN/prefix.<codec>      解码前缀(最近一次 key frame 起)
  camN/frames.jsonl        逐帧 index/时间戳/stride/vstride/y_size/uv_size/au_bytes/key_frame
  session.json             会话元数据与四路计数
```

## 3. 验收判据

```text
SENSOR_CAPTURE_RESULT path=<目录> data_complete=yes cleanup_complete=yes ...
```

且进程退出码为 0、四路 `raw_frames_written` 均等于目标帧数、`session.json` 中 `data_complete=true` 且 `fatal_error` 为空,才是完整采集。

注意:IMU producer 失败会导致进程以非零退出码结束、`data_complete=no`;此时四路 raw/au 写盘仍可能完整,按 `session.json` 逐路计数判断,不直接采信 `data_complete`。

## 4. 离线转换(Host)

```bash
python3 scripts/nv12_tee_extract.py <capture 目录> <输出目录>
```

- 输出目录必须不存在(原子 no-replace)。
- 产物:`camN/raw/NNNNN.jpg`、`camN/encoded/NNNNN.jpg`、`conversion_summary.json`。
- 转换对每个序号重建「前缀 + 该帧及之前全部 AU」的参考链解码;缺 AU 或解码器丢弃的帧不输出 encoded 图,缺口记入 summary,不造成序号错位。

校验 `conversion_summary.json`:

```text
encoded_images_by_camera[cameraN] == 目标帧数
encoded_missing_access_units_by_camera[cameraN] == 0
encoded_skipped_access_units_by_camera[cameraN] == 0
```

## 5. 同序号对齐验证

同序号 `raw/NNNNN.jpg` 与 `encoded/NNNNN.jpg` 应为同一帧。验证方法:两张图缩放到 120x102 灰度、零均值归一化后算 NCC,同帧对齐应 ≥ 0.998。

前提:**场景必须有光照与纹理**。全暗场景下帧间 NCC 本身约 0.9(噪声主导),无法用该阈值判别同帧。

## 6. 编码回调丢帧取证

`session.json` 通道摘要与 `SENSOR_RTSP_RESULT` 行互为印证:

```text
encoded_aus_seen[cameraN]      编码回调到达的 AU 数
encoded_aus_matched[cameraN]   与 raw 成功配对的 AU 数
encoded_aus_unmatched[cameraN] 配对失败的 AU 数

SENSOR_RTSP_RESULT ... preview_dropped_by_camera=cam0:0,...
                        frames_failed_by_camera=cam0:0,...
                        stream_last_error_by_camera=cam0:0,...
```

判定:

- `seen < raw_written`:回调没来,丢在编码/pipeline 层。
- `matched < seen`:回调到达但配对丢,查 recorder 层。
- `preview_dropped_by_camera` 非 0:RTSP send 走降级错误路径,该帧未进编码器。
- `stream_last_error_by_camera` 给出具体错误码。

采集时必须**保留完整 stdout/stderr**(重定向到文件),以上计数行是取证的第一证据。
