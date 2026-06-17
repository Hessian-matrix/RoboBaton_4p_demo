#include "cam_demo_pipeline.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "cam_demo_rtsp.h"

namespace robobaton_demo {

class FrameQueue {
 public:
  // 功能：创建空队列。
  // 输入输出：无。
  FrameQueue() = default;

  FrameQueue(const FrameQueue&) = delete;
  FrameQueue& operator=(const FrameQueue&) = delete;

  // 功能：把一帧已 retain 的图像放入固定容量队列。
  // 输入：item 包含 frame 引用、DMA 地址、时间戳和同步组信息。
  // 输出：true 表示入队成功，false 表示队列已经停止。
  // 副作用：队列满时阻塞调用线程，不丢弃旧帧。
  bool Push(QueuedFrame item) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (size_ == frames_.size()) {
      ++full_wait_count_;
    }

    // 队列满时等待消费者释放槽位，优先保持四路帧序连续。
    not_full_.wait(lock, [&] { return stopped_ || size_ < frames_.size(); });
    if (stopped_) {
      return false;
    }

    // 只有拿到空槽后才写入，避免覆盖仍在等待编码的 retained DMA frame。
    const size_t tail = (head_ + size_) % frames_.size();
    frames_[tail] = item;
    ++size_;
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  // 功能：从队列取出一帧交给消费线程。
  // 输入输出：item 接收出队帧内容。
  // 输出：true 表示取到帧，false 表示队列停止且没有剩余帧。
  // 副作用：释放一个队列槽位并唤醒可能阻塞的生产者。
  bool Pop(QueuedFrame* item) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [&] { return stopped_ || size_ > 0; });

    if (size_ == 0) {
      return false;
    }

    *item = frames_[head_];
    frames_[head_] = QueuedFrame{};
    head_ = (head_ + 1) % frames_.size();
    --size_;
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  // 功能：停止队列并唤醒所有等待线程。
  // 输入输出：无。
  // 副作用：之后 Push 会失败，Pop 会在取完剩余帧后失败。
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  // 功能：读取因队列满而等待的累计次数。
  // 输出：累计等待次数，用于诊断吞吐瓶颈。
  uint64_t full_wait_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return full_wait_count_;
  }

  // 功能：读取当前队列长度。
  // 输出：队列中等待消费的帧数。
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::array<QueuedFrame, kQueueCapacity> frames_{};
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t full_wait_count_ = 0;
  bool stopped_ = false;
};

class GroupSendBarrier {
 public:
  // 功能：创建未配置的同步发送 barrier。
  // 输入输出：无。
  GroupSendBarrier() = default;

  GroupSendBarrier(const GroupSendBarrier&) = delete;
  GroupSendBarrier& operator=(const GroupSendBarrier&) = delete;

  // 功能：配置参与同组发送对齐的物理相机 mask，并重置 barrier 状态。
  // 输入：camera_mask bit0..bit3 对应 cam0..cam3。
  // 输出：无。
  // 副作用：清空等待、到达和已发送状态。
  void Configure(uint32_t camera_mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    all_channels_mask_ = camera_mask & kDefaultCameraMask;
    current_group_id_ = 0;
    arrived_mask_ = 0;
    sent_mask_ = 0;
    waiting_mask_ = 0;
    waiting_group_ids_.fill(0);
    release_current_group_ = false;
    stopped_ = false;
  }

  // 功能：在 RTSP 送帧前等待同一 group_id 的其他通道到齐。
  // 输入：channel 为当前通道，group_id 为 libsc132 帧组编号。
  // 输出：true 表示允许发送当前帧，false 表示停止或该帧组已被跳过。
  // 副作用：快通道会在这里等待慢通道，以降低推流画面跨路相位差。
  bool WaitBeforeSend(int channel, uint64_t group_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint32_t channel_mask = 1U << static_cast<uint32_t>(channel);

    // 2026-06-17 修改原因：单颗 sensor 诊断会启用非连续物理 camera_id，barrier 必须按物理 bitmask 判断到齐，不能按 0..N-1 连续路数遍历。
    waiting_group_ids_[channel] = group_id;
    waiting_mask_ |= channel_mask;
    while (!stopped_ && group_id > current_group_id_) {
      if ((waiting_mask_ | arrived_mask_) == all_channels_mask_) {
        uint64_t min_waiting_group_id = std::numeric_limits<uint64_t>::max();
        for (int i = 0; i < kMaxChannels; ++i) {
          if ((waiting_mask_ & (1U << static_cast<uint32_t>(i))) != 0) {
            min_waiting_group_id = std::min(min_waiting_group_id, waiting_group_ids_[i]);
          }
        }
        if (min_waiting_group_id > current_group_id_) {
          current_group_id_ = min_waiting_group_id;
          arrived_mask_ = 0;
          sent_mask_ = 0;
          release_current_group_ = false;
          group_changed_.notify_all();
          break;
        }
      }
      group_changed_.wait(lock);
    }
    if (stopped_) {
      waiting_mask_ &= ~channel_mask;
      return false;
    }
    if (group_id != current_group_id_) {
      waiting_mask_ &= ~channel_mask;
      return false;
    }

    waiting_mask_ &= ~channel_mask;
    arrived_mask_ |= channel_mask;
    if (arrived_mask_ == all_channels_mask_) {
      release_current_group_ = true;
      group_changed_.notify_all();
    } else {
      // 2026-06-17 修改原因：当前 group 未到齐时等待所有启用物理相机，单颗诊断时不会等待未启用通道。
      group_changed_.wait(lock, [&] {
        return stopped_ || release_current_group_ || group_id != current_group_id_;
      });
    }

    return !stopped_ && group_id == current_group_id_;
  }

  // 功能：标记某一路当前 group 已完成 RTSP 送帧。
  // 输入：channel 为通道号，group_id 为已发送帧组编号。
  // 输出：无。
  // 副作用：所有通道都发送完成后推进到下一个 group。
  void MarkSent(int channel, uint64_t group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (group_id != current_group_id_) {
      return;
    }

    sent_mask_ |= 1U << static_cast<uint32_t>(channel);
    if (sent_mask_ == all_channels_mask_) {
      ++current_group_id_;
      arrived_mask_ = 0;
      sent_mask_ = 0;
      release_current_group_ = false;
      group_changed_.notify_all();
    }
  }

  // 功能：停止 barrier 并唤醒所有等待发送的 worker。
  // 输入输出：无。
  // 副作用：正在 WaitBeforeSend 的线程会返回 false。
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    group_changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable group_changed_;
  uint32_t all_channels_mask_ = 0;
  uint64_t current_group_id_ = 0;
  uint32_t arrived_mask_ = 0;
  uint32_t sent_mask_ = 0;
  uint32_t waiting_mask_ = 0;
  std::array<uint64_t, kMaxChannels> waiting_group_ids_{};
  bool release_current_group_ = false;
  bool stopped_ = false;
};

struct ChannelState {
  FrameQueue queue;
};

// 单路运行诊断状态，由发送 worker 写入，诊断线程读取。
struct ChannelDiagnostics {
  std::atomic<uint64_t> last_sequence{0};
  std::atomic<uint64_t> last_camera_timestamp_ns{0};
  std::atomic<uint64_t> last_rtsp_timestamp_ns{0};
  std::atomic<uint64_t> last_pipeline_delay_ms{0};
  std::atomic<uint64_t> last_send_duration_ns{0};
  std::atomic<uint64_t> total_sent_frames{0};
};

class FramePipeline::Impl {
 public:
  // 功能：创建流水线内部实现并配置发送 barrier。
  // 输入：options 为运行参数副本。
  explicit Impl(Options options) : options_(std::move(options)) {
    group_send_barrier_.Configure(options_.camera_mask);
  }

  // 功能：为每个启用物理相机启动一个 RTSP 发送线程。
  // 输入输出：无。
  // 副作用：线程启动后阻塞等待各自队列中的相机帧。
  void StartWorkers() {
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (CameraMaskContains(options_.camera_mask, camera_id)) {
        workers_[camera_id] = std::thread(&Impl::EncodeWorker, this, camera_id);
      }
    }
  }

  // 功能：按配置启动周期诊断线程。
  // 输入输出：无。
  // 副作用：options_.diagnostics 为 true 时创建后台线程并周期打印同步/发送状态。
  void StartDiagnosticsIfEnabled() {
    if (options_.diagnostics) {
      diagnostics_worker_ =
          std::thread(&Impl::DiagnosticsWorker, this, options_.diagnostic_interval_ms);
    }
  }

  // 功能：构造 libsc132 帧组回调配置。
  // 输入：owner 为 FramePipeline 对象指针，将作为回调 user 传回。
  // 输出：包含启用物理相机数量、sensor 原始尺寸、同步阈值和超时时间的配置结构体。
  sc132_frame_set_config_t MakeFrameSetConfig(FramePipeline* owner) const {
    sc132_frame_set_config_t config{};
    config.cb = FramePipeline::FrameSetCallback;
    config.user = owner;
    config.camera_count = CameraMaskPopCount(options_.camera_mask);
    config.max_skew_ns = options_.frame_set_max_skew_ns;
    config.timeout_ms = options_.frame_set_timeout_ms;
    // 2026-06-17 修改原因：对外 width/height 是正装输出尺寸；libsc132 内部仍按 sensor 原始采集尺寸配置。
    config.width = kSensorInputWidth;
    config.height = kSensorInputHeight;
    return config;
  }

  // 功能：通知发送 barrier 和所有通道队列停止。
  // 输入输出：无。
  // 副作用：等待中的 worker 会被唤醒并进入退出流程。
  void Stop() {
    group_send_barrier_.Stop();
    for (ChannelState& state : channels_) {
      state.queue.Stop();
    }
  }

  // 功能：等待所有后台线程退出。
  // 输入输出：无。
  // 副作用：调用后 worker 和诊断线程都不再访问 pipeline 内部状态。
  void Join() {
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }

    if (diagnostics_worker_.joinable()) {
      diagnostics_worker_.join();
    }
  }

  // 功能：处理 libsc132 输出的同步帧组。
  // 输入：frame_set 为已按 group_id 配组的多路相机帧。
  // 输出：无。
  // 副作用：调用用户同步 hook，并把每路帧 retain 后放入对应发送队列。
  void HandleFrameSet(const sc132_frame_set_t& frame_set) {
    if (frame_set.camera_count > kMaxChannels) {
      std::cerr << "invalid frame-set camera_count=" << frame_set.camera_count << ", drop\n";
      return;
    }

    // 只有 libsc132 已配组的帧会进入用户同步入口和 RTSP 发送队列。
    OnSynchronizedFrameSet(frame_set);
    if (options_.diagnostics) {
      ReportFrameSetDiagnostic(frame_set);
    }
    for (uint32_t i = 0; i < frame_set.camera_count; ++i) {
      const sc132_frame_set_item_t& item = frame_set.items[i];
      const int camera_id = static_cast<int>(item.camera_id);
      if (camera_id < 0 || camera_id >= kMaxChannels || item.frame == nullptr ||
          !CameraMaskContains(options_.camera_mask, camera_id)) {
        std::cerr << "invalid frame-set item camera_id=" << item.camera_id << ", skip\n";
        continue;
      }
      EnqueueCameraFrame(camera_id, item.frame, frame_set.group_id,
                         frame_set.group_timestamp_ns, frame_set.max_skew_ns,
                         item.sequence);
    }
  }

 private:
  // 功能：按固定间隔打印帧组级同步诊断。
  // 输入：frame_set 为当前同步帧组。
  // 输出：无。
  // 副作用：仅 diagnostics 开启时写 stdout，用于回归脚本检查 frame_id 和 timestamp。
  void ReportFrameSetDiagnostic(const sc132_frame_set_t& frame_set) {
    if (frame_set.camera_count == 0 || frame_set.camera_count > kMaxChannels) {
      return;
    }

    // 诊断按秒采样，避免 stdout 影响 60fps 采集和编码热路径。
    static uint64_t last_report_ns = 0;
    const uint64_t now_ns = SteadyClockNowNs();
    if (last_report_ns != 0 && now_ns - last_report_ns < 1000000000ULL) {
      return;
    }
    last_report_ns = now_ns;

    std::array<uint64_t, kMaxChannels> camera_ts{};
    std::array<uint32_t, kMaxChannels> frame_ids{};
    std::array<uint64_t, kMaxChannels> sequences{};
    uint64_t newest_ts = 0;
    uint64_t oldest_ts = std::numeric_limits<uint64_t>::max();
    uint32_t mask = 0;

    // 按 camera_id 写入固定槽位，便于人工阅读和脚本按 group_id 聚合校验。
    for (uint32_t i = 0; i < frame_set.camera_count; ++i) {
      const sc132_frame_set_item_t& item = frame_set.items[i];
      if (item.camera_id >= kMaxChannels) {
        continue;
      }
      const int channel = static_cast<int>(item.camera_id);
      camera_ts[channel] = item.timestamp_ns;
      frame_ids[channel] = item.frame_id;
      sequences[channel] = item.sequence;
      mask |= 1U << static_cast<uint32_t>(channel);
      newest_ts = std::max(newest_ts, item.timestamp_ns);
      oldest_ts = std::min(oldest_ts, item.timestamp_ns);
    }

    if (mask != options_.camera_mask || oldest_ts == std::numeric_limits<uint64_t>::max()) {
      return;
    }

    std::cout << "frameset group_id=" << frame_set.group_id
              << " group_ts_ns=" << frame_set.group_timestamp_ns
              << " group_skew_ns=" << frame_set.max_skew_ns
              << " calc_skew_ns=" << (newest_ts - oldest_ts);
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(mask, camera_id)) {
        continue;
      }
      std::cout << " cam" << camera_id
                << "(seq=" << sequences[camera_id]
                << ",frame_id=" << frame_ids[camera_id]
                << ",camera_ts_ns=" << camera_ts[camera_id] << ")";
    }
    std::cout << "\n";
  }

  // 功能：把单路相机帧封装成 QueuedFrame 并放入发送队列。
  // 输入：channel 为通道号；frame 为 libsc132 帧；group_* 为同步帧组元数据。
  // 输出：无。
  // 副作用：成功入队前会 retain frame；队列停止或入队失败时立即 release。
  void EnqueueCameraFrame(int channel, sc132_frame_t* frame, uint64_t group_id,
                          uint64_t group_timestamp_ns, uint64_t group_max_skew_ns,
                          uint64_t group_sequence) {
    if (frame == nullptr || g_stop_requested.load()) {
      return;
    }

    const hb_mem_graphic_buf_t* graph_buf = sc132_frame_get_graphic_buf(frame);
    if (graph_buf == nullptr || graph_buf->virt_addr[0] == nullptr ||
        graph_buf->virt_addr[1] == nullptr || graph_buf->phys_addr[0] == 0 ||
        graph_buf->phys_addr[1] == 0) {
      std::cerr << "cam" << channel << " invalid NV12 DMA frame, drop\n";
      return;
    }

    if (sc132_frame_retain(frame) != 0) {
      std::cerr << "cam" << channel << " retain frame failed, drop\n";
      return;
    }

    QueuedFrame item;
    item.frame = frame;
    item.channel = channel;
    item.sequence = group_sequence;
    item.frame_id = sc132_frame_get_frame_id(frame);
    item.group_id = group_id;
    item.group_timestamp_ns = group_timestamp_ns;
    item.group_max_skew_ns = group_max_skew_ns;
    item.camera_timestamp_ns = sc132_frame_get_timestamp_ns(frame);
    item.rtsp_timestamp_ns = group_timestamp_ns;
    item.enqueue_timestamp_ns = SteadyClockNowNs();
    item.y_data = graph_buf->virt_addr[0];
    item.uv_data = graph_buf->virt_addr[1];
    item.y_phys = graph_buf->phys_addr[0];
    item.uv_phys = graph_buf->phys_addr[1];
    item.y_len = static_cast<long>(graph_buf->size[0]);
    item.uv_len = static_cast<long>(graph_buf->size[1]);
    item.width = sc132_frame_get_width(frame);
    item.height = sc132_frame_get_height(frame);

    // 相机回调线程只做必要元数据封装和入队，耗时编码放到后台 worker。
    if (!channels_[channel].queue.Push(item)) {
      ReleaseQueuedFrame(&item);
    }
  }

  // 功能：消费单路队列，并按同步组顺序送入对应 RTSP endpoint。
  // 输入：channel 为 worker 负责的相机通道。
  // 输出：无。
  // 副作用：调用用户单帧 hook、RTSP 送帧接口，并在每帧处理后 release frame。
  void EncodeWorker(int channel) {
    uint64_t sent_count = 0;
    uint64_t send_duration_total_ns = 0;
    uint64_t send_duration_max_ns = 0;
    auto last_report = std::chrono::steady_clock::now();
    bool first_frame = true;
    const RtspEndpoint rtsp_endpoint = RtspEndpointForChannel(channel);
    const int rtsp_port = RtspPortForChannel(channel);

    while (true) {
      QueuedFrame frame;
      if (!channels_[channel].queue.Pop(&frame)) {
        break;
      }

      // 用户单帧 hook 在后台线程执行，避免阻塞 libsc132 相机回调线程。
      OnQueuedCameraFrame(frame);

      if (!group_send_barrier_.WaitBeforeSend(channel, frame.group_id)) {
        ReleaseQueuedFrame(&frame);
        break;
      }

      if (first_frame) {
        std::cout << "cam" << channel << " first frame"
                  << " seq=" << frame.sequence
                  << " group_id=" << frame.group_id
                  << " group_ts_ns=" << frame.group_timestamp_ns
                  << " group_skew_ns=" << frame.group_max_skew_ns
                  << " frame_id=" << frame.frame_id
                  << " camera_ts_ns=" << frame.camera_timestamp_ns
                  << " rtsp_ts_ns=" << frame.rtsp_timestamp_ns
                  << " y_phys=0x" << std::hex << frame.y_phys
                  << " uv_phys=0x" << frame.uv_phys << std::dec
                  << " y_len=" << frame.y_len
                  << " uv_len=" << frame.uv_len
                  << " input_size=" << frame.width << "x" << frame.height
                  << " rtsp_output_size=" << OutputWidth(options_) << "x" << OutputHeight(options_)
                  << " rtsp_endpoint=" << RtspEndpointName(rtsp_endpoint)
                  << " rtsp_port=" << rtsp_port << "\n";
        first_frame = false;
      }

      const uint64_t send_begin_ns = SteadyClockNowNs();
      SendFrameToRtspEndpoint(rtsp_endpoint, frame);
      const uint64_t send_end_ns = SteadyClockNowNs();
      const uint64_t send_duration_ns = send_end_ns - send_begin_ns;
      group_send_barrier_.MarkSent(channel, frame.group_id);
      ++sent_count;
      send_duration_total_ns += send_duration_ns;
      send_duration_max_ns = std::max(send_duration_max_ns, send_duration_ns);

      const uint64_t pipeline_delay_ms = (send_end_ns - frame.enqueue_timestamp_ns) / 1000000ULL;
      diagnostics_[channel].last_sequence.store(frame.sequence, std::memory_order_relaxed);
      diagnostics_[channel].last_camera_timestamp_ns.store(frame.camera_timestamp_ns,
                                                           std::memory_order_relaxed);
      diagnostics_[channel].last_rtsp_timestamp_ns.store(frame.rtsp_timestamp_ns,
                                                         std::memory_order_relaxed);
      diagnostics_[channel].last_pipeline_delay_ms.store(pipeline_delay_ms,
                                                         std::memory_order_relaxed);
      diagnostics_[channel].last_send_duration_ns.store(send_duration_ns,
                                                        std::memory_order_relaxed);
      diagnostics_[channel].total_sent_frames.fetch_add(1, std::memory_order_relaxed);

      ReportChannelStats(channel, frame, sent_count, send_duration_total_ns,
                         send_duration_max_ns, pipeline_delay_ms, &last_report);
      ReleaseQueuedFrame(&frame);
    }

    std::cout << "cam" << channel << " worker stopped\n";
  }

  // 功能：打印单路 RTSP 发送统计。
  // 输入：channel、最近一帧 frame、累计发送数量和耗时统计。
  // 输出：无。
  // 副作用：达到诊断间隔时写 stdout，并清零本周期累计计数。
  void ReportChannelStats(int channel, const QueuedFrame& frame, uint64_t& sent_count,
                          uint64_t& send_duration_total_ns, uint64_t& send_duration_max_ns,
                          uint64_t pipeline_delay_ms,
                          std::chrono::steady_clock::time_point* last_report) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(now - *last_report).count();
    if (elapsed_s < static_cast<double>(options_.diagnostic_interval_ms) / 1000.0) {
      return;
    }

    const RtspEndpoint rtsp_endpoint = RtspEndpointForChannel(channel);
    const int rtsp_port = RtspPortForChannel(channel);
    const double fps = static_cast<double>(sent_count) / elapsed_s;
    const double send_avg_ms =
        sent_count == 0 ? 0.0
                        : static_cast<double>(send_duration_total_ns) /
                              static_cast<double>(sent_count) / 1000000.0;
    const double send_max_ms = static_cast<double>(send_duration_max_ns) / 1000000.0;
    std::cout << std::fixed << std::setprecision(2)
              << "cam" << channel
              << " fps=" << fps
              << " last_seq=" << frame.sequence
              << " group_id=" << frame.group_id
              << " group_skew_ns=" << frame.group_max_skew_ns
              << " queue=" << channels_[channel].queue.size() << "/" << kQueueCapacity
              << " full_waits=" << channels_[channel].queue.full_wait_count()
              << " pipeline_delay_ms=" << pipeline_delay_ms
              << " camera_ts_ns=" << frame.camera_timestamp_ns
              << " rtsp_ts_ns=" << frame.rtsp_timestamp_ns;
    if (options_.diagnostics) {
      std::cout << " send_avg_ms=" << send_avg_ms
                << " send_max_ms=" << send_max_ms
                << " rtsp_endpoint=" << RtspEndpointName(rtsp_endpoint)
                << " rtsp_port=" << rtsp_port;
    }
    std::cout << "\n";
    sent_count = 0;
    send_duration_total_ns = 0;
    send_duration_max_ns = 0;
    *last_report = now;
  }

  // 功能：周期性汇总启用物理相机最近已发送帧的同步和延迟状态。
  // 输入：interval_ms 为诊断间隔。
  // 输出：无。
  // 副作用：写 stdout，帮助区分采集相位差、队列延迟和 RTSP 发送耗时。
  void DiagnosticsWorker(int interval_ms) {
    while (!g_stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      if (g_stop_requested.load()) {
        break;
      }

      std::array<uint64_t, kMaxChannels> camera_ts_snapshot{};
      std::array<uint64_t, kMaxChannels> rtsp_ts_snapshot{};
      std::array<uint64_t, kMaxChannels> send_duration_snapshot{};
      std::array<uint64_t, kMaxChannels> pipeline_delay_snapshot{};
      std::array<uint64_t, kMaxChannels> sequence_snapshot{};
      uint64_t newest_camera_ts_ns = 0;
      uint64_t oldest_camera_ts_ns = std::numeric_limits<uint64_t>::max();
      uint64_t newest_rtsp_ts_ns = 0;
      uint64_t oldest_rtsp_ts_ns = std::numeric_limits<uint64_t>::max();
      bool all_enabled_cameras_have_frames = true;
      // 2026-06-17 修改原因：内部单颗诊断可能只启用 cam1/cam2/cam3，诊断采样必须按物理 mask 遍历而不是按 0..channels-1。
      for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
        if (!CameraMaskContains(options_.camera_mask, camera_id)) {
          continue;
        }
        const uint64_t ts = diagnostics_[camera_id].last_camera_timestamp_ns.load(
            std::memory_order_relaxed);
        const uint64_t rtsp_ts = diagnostics_[camera_id].last_rtsp_timestamp_ns.load(
            std::memory_order_relaxed);
        if (ts == 0 || rtsp_ts == 0) {
          all_enabled_cameras_have_frames = false;
          break;
        }
        camera_ts_snapshot[camera_id] = ts;
        rtsp_ts_snapshot[camera_id] = rtsp_ts;
        send_duration_snapshot[camera_id] =
            diagnostics_[camera_id].last_send_duration_ns.load(std::memory_order_relaxed);
        pipeline_delay_snapshot[camera_id] =
            diagnostics_[camera_id].last_pipeline_delay_ms.load(std::memory_order_relaxed);
        sequence_snapshot[camera_id] =
            diagnostics_[camera_id].last_sequence.load(std::memory_order_relaxed);
        newest_camera_ts_ns = std::max(newest_camera_ts_ns, ts);
        oldest_camera_ts_ns = std::min(oldest_camera_ts_ns, ts);
        newest_rtsp_ts_ns = std::max(newest_rtsp_ts_ns, rtsp_ts);
        oldest_rtsp_ts_ns = std::min(oldest_rtsp_ts_ns, rtsp_ts);
      }
      if (!all_enabled_cameras_have_frames) {
        continue;
      }

      // 同时输出相机原始时间戳 skew 和 RTSP PTS skew，定位不同层级的不同步来源。
      const double camera_skew_ms =
          static_cast<double>(newest_camera_ts_ns - oldest_camera_ts_ns) / 1000000.0;
      const double rtsp_pts_skew_ms =
          static_cast<double>(newest_rtsp_ts_ns - oldest_rtsp_ts_ns) / 1000000.0;
      std::cout << std::fixed << std::setprecision(2)
                << "[diag] rtsp_pts_skew_ms=" << rtsp_pts_skew_ms
                << " camera_latest_skew_ms=" << camera_skew_ms;
      for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
        if (!CameraMaskContains(options_.camera_mask, camera_id)) {
          continue;
        }
        const double camera_lag_ms =
            static_cast<double>(newest_camera_ts_ns - camera_ts_snapshot[camera_id]) / 1000000.0;
        const double rtsp_lag_ms =
            static_cast<double>(newest_rtsp_ts_ns - rtsp_ts_snapshot[camera_id]) / 1000000.0;
        const double send_ms =
            static_cast<double>(send_duration_snapshot[camera_id]) / 1000000.0;
        std::cout << " cam" << camera_id
                  << "(port=" << RtspPortForChannel(camera_id)
                  << ",camera_lag_ms=" << camera_lag_ms
                  << ",rtsp_lag_ms=" << rtsp_lag_ms
                  << ",send_ms=" << send_ms
                  << ",pipe_ms=" << pipeline_delay_snapshot[camera_id]
                  << ",seq=" << sequence_snapshot[camera_id] << ")";
      }
      std::cout << "\n";
    }
  }

  Options options_;
  std::array<ChannelState, kMaxChannels> channels_;
  std::array<ChannelDiagnostics, kMaxChannels> diagnostics_;
  GroupSendBarrier group_send_barrier_;
  std::array<std::thread, kMaxChannels> workers_;
  std::thread diagnostics_worker_;
};

// 功能：创建帧处理流水线对象。
// 输入：options 为运行参数副本。
// 输出：FramePipeline 实例持有内部实现对象。
FramePipeline::FramePipeline(Options options)
    : impl_(new Impl(std::move(options))) {}

// 功能：销毁帧处理流水线。
// 输入输出：无。
// 副作用：兜底停止并回收后台线程。
FramePipeline::~FramePipeline() {
  // 析构兜底停止并回收线程，避免初始化中途异常导致 joinable thread 触发 terminate。
  Stop();
  Join();
}

// 功能：启动每路 RTSP 发送 worker。
// 输入输出：无。
// 副作用：创建后台线程。
void FramePipeline::StartWorkers() {
  impl_->StartWorkers();
}

// 功能：按配置启动诊断线程。
// 输入输出：无。
// 副作用：diagnostics 开启时创建后台线程。
void FramePipeline::StartDiagnosticsIfEnabled() {
  impl_->StartDiagnosticsIfEnabled();
}

// 功能：生成 libsc132 帧组回调配置。
// 输入：无。
// 输出：sc132_frame_set_config_t，user 指针绑定当前 FramePipeline。
sc132_frame_set_config_t FramePipeline::MakeFrameSetConfig() {
  return impl_->MakeFrameSetConfig(this);
}

// 功能：通知 pipeline 停止。
// 输入输出：无。
// 副作用：唤醒队列和发送 barrier 中等待的线程。
void FramePipeline::Stop() {
  impl_->Stop();
}

// 功能：等待后台线程退出。
// 输入输出：无。
// 副作用：阻塞直到 worker 和诊断线程全部 join 完成。
void FramePipeline::Join() {
  impl_->Join();
}

// 功能：libsc132 帧组回调入口。
// 输入：frame_set 为 libsc132 输出的同步帧组；user 为 FramePipeline 指针。
// 输出：无。
// 副作用：把同步帧组转交给 pipeline，异常路径会丢弃无效回调。
void FramePipeline::FrameSetCallback(const sc132_frame_set_t* frame_set, void* user) {
  if (frame_set == nullptr || g_stop_requested.load()) {
    return;
  }
  FramePipeline* pipeline = static_cast<FramePipeline*>(user);
  if (pipeline == nullptr) {
    std::cerr << "frame-set callback missing pipeline, drop\n";
    return;
  }
  pipeline->impl_->HandleFrameSet(*frame_set);
}

}  // namespace robobaton_demo
