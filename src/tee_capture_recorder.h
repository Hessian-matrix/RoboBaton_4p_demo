#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "cam_demo_common.h"
#include "prrtsp_v2.h"

namespace robobaton_demo {

// tee 采集会话顶层元数据文件的 schema 标识。
inline constexpr const char* kTeeCaptureSessionSchema =
    "robobaton_tee_capture_session_v1";

struct TeeCaptureRecorderStats {
  uint64_t raw_frames_retained = 0U;
  uint64_t raw_retain_failures = 0U;
  uint64_t raw_frames_written = 0U;
  uint64_t raw_frames_quota_dropped = 0U;
  uint64_t raw_frames_queue_dropped = 0U;
  uint64_t warmup_skipped_frames = 0U;
  uint64_t encoded_aus_seen = 0U;
  uint64_t encoded_aus_matched = 0U;
  uint64_t encoded_aus_unmatched = 0U;
  uint64_t prefix_aus_buffered = 0U;
  uint64_t prefix_overflow_events = 0U;
  std::array<uint64_t, kMaxChannels> raw_frames_written_by_camera{};
  std::array<uint64_t, kMaxChannels> encoded_aus_matched_by_camera{};
};

struct TeeCaptureFinishResult {
  bool data_complete = false;
  bool cleanup_complete = false;
  std::string error;

  operator bool() const noexcept { return data_complete && cleanup_complete; }
};

// 同进程 tee 采集器：sensor_demo 开启后，从相机完全启动起再等待
// capture_warmup_seconds，随后对每路连续保存 capture_frame_count 帧的
// 紧凑 NV12 plane 字节与 RTSP 编码 access unit。开关关闭时 Start 不会被
// 调用，本类完全不参与帧路径，原有 RTSP/MP4/bag 语义不变。
//
// 线程模型：ObserveRawFrame 在 pipeline worker 线程调用，只 retain 一次
// 并复制元数据入队；ObserveEncodedFrame 在 RTSP 编码回调线程调用，AU 只
// 借用到回调返回，立即复制进每路缓冲。像素拷贝与文件写盘全部在私有
// writer 线程完成，不阻塞帧路径。
class TeeCaptureRecorder final {
 public:
  TeeCaptureRecorder();
  ~TeeCaptureRecorder();

  TeeCaptureRecorder(const TeeCaptureRecorder&) = delete;
  TeeCaptureRecorder& operator=(const TeeCaptureRecorder&) = delete;

  // 在 sc132 frame-set 启动成功后调用；创建输出目录骨架并固定 warmup
  // 结束时刻。目录创建失败抛出 std::runtime_error。
  void Start(const Options& options);

  // pipeline on_queued_frame hook：warmup 结束后对每路保留 capture_frame_count
  // 帧；只 retain 一次并入队，不拷贝像素、不写盘。
  void ObserveRawFrame(int camera_id, const QueuedFrame& frame) noexcept;

  // RTSP encoded observer：复制 access unit 到每路缓冲；解码前缀按最近
  // key frame 重置，保证目标帧可从该前缀起解码。
  void ObserveEncodedFrame(int camera_id,
                           const prrtsp_encoded_frame_v2& frame) noexcept;

  // RTSP close 结束后调用：停 writer、release 全部 retained 帧、写
  // frames.jsonl/prefix.h264/session.json。返回数据是否完整。
  TeeCaptureFinishResult Finish(bool session_success) noexcept;
  void Abort() noexcept;

  bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
  bool HasFatalError() const noexcept;
  std::string ErrorMessage() const;
  TeeCaptureRecorderStats SnapshotStats() const;

#ifdef RELEASE008_TESTING
  // 测试钩子：warmup 置 0 表示 Start 后立即进入采集窗口。
  void SetWarmupSecondsForTest(uint32_t seconds) noexcept;
  void SetCaptureFrameCountForTest(uint32_t count) noexcept;
  void FailRawWriteForTest() noexcept;
  void FailEncodedWriteForTest() noexcept;
  uint64_t PendingRawJobsForTest(int camera_id) const noexcept;
  uint64_t PendingEncodedAusForTest(int camera_id) const noexcept;
#endif

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> enabled_{false};
};

}  // namespace robobaton_demo
