#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "cam_demo_common.h"

namespace robobaton_demo {

// 原始帧响应头：96 字节、小端、packed。请求方按此布局解包后接紧凑 NV12 payload
// （Y 逐行按 stride 截取 width，UV 交错同样逐行截取 width；总长 width*height*3/2）。
// status==0 时 payload_size>0 且后接 payload；其余状态 payload_size==0 且无 payload。
#pragma pack(push, 1)
struct RawFrameResponseHeader {
  uint32_t magic;          // "RWF1" = 0x31574652
  uint32_t version;        // 1
  int32_t status;          // 0 ok / 1 no-match / 2 bad-request / 3 camera-disabled / 4 no-frame
  int32_t camera_id;
  uint64_t group_timestamp_ns;
  uint64_t camera_timestamp_ns;
  uint64_t frame_id;
  uint64_t group_id;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t vstride;
  uint64_t payload_size;
  uint64_t reserved[3];
};
#pragma pack(pop)

inline constexpr uint32_t kRawFrameResponseMagic = 0x31574652U;
inline constexpr uint32_t kRawFrameResponseVersion = 1U;

// 单请求/单响应协议状态码；仅 kOk 附带 payload。
enum class RawFrameResponseStatus : int32_t {
  kOk = 0,
  kNoMatch = 1,
  kBadRequest = 2,
  kCameraDisabled = 3,
  kNoFrame = 4,
};

struct RawFrameServerStats {
  uint64_t raw_frames_retained = 0U;
  uint64_t raw_retain_failures = 0U;
  uint64_t raw_frames_copied = 0U;
  uint64_t raw_frames_queue_dropped = 0U;
  uint64_t requests_total = 0U;
  uint64_t requests_matched = 0U;
  uint64_t requests_no_match = 0U;
  uint64_t requests_latest = 0U;
  uint64_t requests_bad = 0U;
  std::array<uint64_t, kMaxChannels> raw_frames_copied_by_camera{};
};

// 时间戳寻址的 raw 帧服务：接受单请求/单响应 TCP 连接，返回与请求
// group_timestamp_ns 在最近邻半帧容差内配对的紧凑 NV12 原帧，或按 LATEST
// 返回最近写入帧。帧在 pipeline on_queued_frame hook 只 retain 一次并入队，
// 私有 writer 线程做像素拷贝进每路 ring 后 release，不阻塞帧路径；accept
// 线程独立应答。开关关闭时 Start 不被调用，本类完全不参与帧路径。
//
// 请求行（换行结尾）：
//   GET <camera_id> <group_timestamp_ns>
//   LATEST <camera_id>
// 响应：RawFrameResponseHeader 后接紧凑 NV12 payload（status==0 时）。
class RawFrameServer final {
 public:
  RawFrameServer();
  ~RawFrameServer();

  RawFrameServer(const RawFrameServer&) = delete;
  RawFrameServer& operator=(const RawFrameServer&) = delete;

  // 绑定 listen socket 并起 writer/accept 线程；绑定失败抛出 std::runtime_error。
  void Start(const Options& options);

  // pipeline on_queued_frame hook：只 retain 一次并复制元数据入队，不做像素拷贝。
  void ObserveRawFrame(int camera_id, const QueuedFrame& frame) noexcept;

  // 关闭 accept 后停止 writer：release 全部 retained 帧、停止应答。
  void Stop() noexcept;

  bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
  bool HasFatalError() const noexcept;
  std::string ErrorMessage() const;
  RawFrameServerStats SnapshotStats() const;

#ifdef RELEASE008_TESTING
  void SetRingDepthForTest(uint32_t depth) noexcept;
  uint64_t PendingRawJobsForTest(int camera_id) const noexcept;
  int BoundPortForTest() const noexcept;
#endif

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> enabled_{false};
};

}  // namespace robobaton_demo
