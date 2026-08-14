#include <signal.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>

extern "C" {
#include "icm42688_driver.h"
#include "prrtsp_v2.h"
#include "sc132camera.h"
}

#include "cam_demo_common.h"
#include "cam_demo_config.h"
#include "cam_demo_pipeline.h"
#include "cam_demo_rtsp.h"
#include "sensor_bag_recorder.h"

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

#ifdef RELEASE008_TESTING
namespace robobaton_demo {
struct SensorDemoImuStatsTestResult {
  uint32_t timing_sample_drops = 0U;
  uint32_t max_consecutive_drops = 0U;
  bool max_consecutive_drops_valid = false;
};
}  // namespace robobaton_demo
#endif

namespace {

volatile sig_atomic_t g_signal_stop = 0;

void SignalHandler(int) {
  g_signal_stop = 1;
}

#ifdef RELEASE008_TESTING
bool InjectJoinFailure(std::thread&, void*) { return false; }
#endif

void ObserveFrameSetForBag(const sc132_frame_set_t& frame_set, void* user) {
  auto* recorder = static_cast<robobaton_demo::SensorBagRecorder*>(user);
  if (recorder != nullptr) {
    static_cast<void>(recorder->TryAcceptFrameSet(frame_set));
  }
}

std::ostream& AppendBagLatencySummary(std::ostream& stream, const char* prefix,
                                      const robobaton_demo::SensorBagLatencyStats& stats) {
  return stream << ' ' << prefix << "_count=" << stats.count << ' ' << prefix
                << "_avg_ns=" << robobaton_demo::SensorBagLatencyAverageNs(stats) << ' '
                << prefix << "_p50_ns="
                << robobaton_demo::SensorBagLatencyPercentileUpperNs(stats, 50U) << ' '
                << prefix << "_p95_ns="
                << robobaton_demo::SensorBagLatencyPercentileUpperNs(stats, 95U) << ' '
                << prefix << "_p99_ns="
                << robobaton_demo::SensorBagLatencyPercentileUpperNs(stats, 99U) << ' '
                << prefix << "_max_ns=" << stats.max_ns;
}

std::ostream& AppendRosbagWriterLatencySummary(
    std::ostream& stream, const char* prefix,
    const robobaton_demo::RosbagV2LatencyStats& stats) {
  return stream << ' ' << prefix << "_count=" << stats.count << ' ' << prefix
                << "_avg_ns=" << robobaton_demo::RosbagV2LatencyAverageNs(stats) << ' '
                << prefix << "_p50_ns="
                << robobaton_demo::RosbagV2LatencyPercentileUpperNs(stats, 50U) << ' '
                << prefix << "_p95_ns="
                << robobaton_demo::RosbagV2LatencyPercentileUpperNs(stats, 95U) << ' '
                << prefix << "_p99_ns="
                << robobaton_demo::RosbagV2LatencyPercentileUpperNs(stats, 99U) << ' '
                << prefix << "_max_ns=" << stats.max_ns;
}

std::ostream& AppendRosbagWriterSummary(
    std::ostream& stream, const robobaton_demo::RosbagV2WriterStats& stats) {
  AppendRosbagWriterLatencySummary(stream, "chunk_open", stats.chunk_open_latency);
  AppendRosbagWriterLatencySummary(stream, "chunk_write", stats.chunk_write_latency);
  AppendRosbagWriterLatencySummary(stream, "chunk_header_patch",
                                   stats.chunk_header_patch_latency);
  AppendRosbagWriterLatencySummary(stream, "chunk_index", stats.chunk_index_latency);
  AppendRosbagWriterLatencySummary(stream, "chunk_close", stats.chunk_close_latency);
  AppendRosbagWriterLatencySummary(stream, "record_write", stats.record_write_latency);
  AppendRosbagWriterLatencySummary(stream, "record_header_write",
                                   stats.record_header_write_latency);
  AppendRosbagWriterLatencySummary(stream, "record_payload_write",
                                   stats.record_payload_write_latency);
  AppendRosbagWriterLatencySummary(stream, "flush_close", stats.flush_close_latency);
  return stream << " raw_write_calls=" << stats.raw_write_calls
                << " raw_write_bytes=" << stats.raw_write_bytes
                << " record_header_write_bytes=" << stats.record_header_write_bytes
                << " record_payload_write_bytes=" << stats.record_payload_write_bytes;
}

uint64_t SensorDemoSteadyNowNs() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct ProcMeminfoSnapshot {
  uint64_t dirty_kb = 0U;
  uint64_t writeback_kb = 0U;
  bool valid = false;
};

struct ProcDiskstatsSnapshot {
  uint64_t read_ios = 0U;
  uint64_t write_ios = 0U;
  uint64_t read_sectors = 0U;
  uint64_t write_sectors = 0U;
  uint64_t io_time_ms = 0U;
  bool valid = false;
};

struct DiskObservabilitySnapshot {
  ProcMeminfoSnapshot meminfo;
  ProcDiskstatsSnapshot diskstats;
  uint64_t steady_ns = 0U;
};

uint64_t NondecreasingDelta(uint64_t before, uint64_t after) noexcept {
  return after >= before ? after - before : 0U;
}

ProcMeminfoSnapshot ReadProcMeminfo() {
  std::ifstream input("/proc/meminfo");
  ProcMeminfoSnapshot snapshot;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string key;
    uint64_t value_kb = 0U;
    if (!(fields >> key >> value_kb)) {
      continue;
    }
    if (key == "Dirty:") {
      snapshot.dirty_kb = value_kb;
    } else if (key == "Writeback:") {
      snapshot.writeback_kb = value_kb;
    }
  }
  snapshot.valid = static_cast<bool>(input.eof());
  return snapshot;
}

ProcDiskstatsSnapshot ReadProcDiskstats() {
  std::ifstream input("/proc/diskstats");
  ProcDiskstatsSnapshot snapshot;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    unsigned int major = 0U;
    unsigned int minor = 0U;
    std::string name;
    uint64_t reads_completed = 0U;
    uint64_t reads_merged = 0U;
    uint64_t sectors_read = 0U;
    uint64_t read_time_ms = 0U;
    uint64_t writes_completed = 0U;
    uint64_t writes_merged = 0U;
    uint64_t sectors_written = 0U;
    uint64_t write_time_ms = 0U;
    uint64_t io_in_progress = 0U;
    uint64_t io_time_ms = 0U;
    if (!(fields >> major >> minor >> name >> reads_completed >> reads_merged >>
          sectors_read >> read_time_ms >> writes_completed >> writes_merged >>
          sectors_written >> write_time_ms >> io_in_progress >> io_time_ms)) {
      continue;
    }
    snapshot.read_ios += reads_completed;
    snapshot.write_ios += writes_completed;
    snapshot.read_sectors += sectors_read;
    snapshot.write_sectors += sectors_written;
    snapshot.io_time_ms += io_time_ms;
  }
  snapshot.valid = static_cast<bool>(input.eof());
  return snapshot;
}

DiskObservabilitySnapshot CaptureDiskObservability() {
  DiskObservabilitySnapshot snapshot;
  snapshot.meminfo = ReadProcMeminfo();
  snapshot.diskstats = ReadProcDiskstats();
  snapshot.steady_ns = SensorDemoSteadyNowNs();
  return snapshot;
}

std::ostream& AppendDiskObservabilitySummary(
    std::ostream& stream, const DiskObservabilitySnapshot& before,
    const DiskObservabilitySnapshot& after) {
  return stream << " dirty_kb=" << after.meminfo.dirty_kb
                << " writeback_kb=" << after.meminfo.writeback_kb
                << " meminfo_available=" << (after.meminfo.valid ? "yes" : "no")
                << " diskstats_available=" << (after.diskstats.valid ? "yes" : "no")
                << " diskstats_interval_ns="
                << NondecreasingDelta(before.steady_ns, after.steady_ns)
                << " diskstats_read_ios_delta="
                << NondecreasingDelta(before.diskstats.read_ios, after.diskstats.read_ios)
                << " diskstats_write_ios_delta="
                << NondecreasingDelta(before.diskstats.write_ios, after.diskstats.write_ios)
                << " diskstats_read_sectors_delta="
                << NondecreasingDelta(before.diskstats.read_sectors,
                                      after.diskstats.read_sectors)
                << " diskstats_write_sectors_delta="
                << NondecreasingDelta(before.diskstats.write_sectors,
                                      after.diskstats.write_sectors)
                << " diskstats_io_time_ms_delta="
                << NondecreasingDelta(before.diskstats.io_time_ms,
                                      after.diskstats.io_time_ms);
}

std::string TerminalToken(const std::string& value) {
  if (value.empty()) {
    return "-";
  }
  std::string token;
  token.reserve(value.size());
  for (unsigned char ch : value) {
    const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' ||
                      ch == '_' || ch == '-' || ch == ':';
    token.push_back(safe ? static_cast<char>(ch) : '_');
  }
  return token;
}

robobaton_demo::PipelineHooks MainPipelineHooks(
    robobaton_demo::SensorBagRecorder* recorder) {
  robobaton_demo::PipelineHooks hooks{};
  if (recorder != nullptr) {
    hooks.on_frame_set = ObserveFrameSetForBag;
    hooks.user = recorder;
  }
#ifdef RELEASE008_TESTING
  const char* inject = std::getenv("RELEASE008_TEST_JOIN_FAILURE");
  if (inject != nullptr && inject[0] != '\0') {
    hooks.join_thread = InjectJoinFailure;
  }
#endif
  return hooks;
}


struct ImuStats {
  uint64_t samples = 0U;
  uint64_t invalid_samples = 0U;
  uint64_t timestamp_duplicates = 0U;
  uint64_t timestamp_regressions = 0U;
  uint64_t first_timestamp_ns = 0U;
  uint64_t last_timestamp_ns = 0U;
  uint64_t min_dt_ns = 0U;
  uint64_t max_dt_ns = 0U;
  uint64_t dt_sum_ns = 0U;
  uint32_t timing_sample_drops = 0U;
  uint32_t max_consecutive_drops = 0U;
  uint32_t max_timestamp_uncertainty_us = 0U;
  uint32_t previous_mapper_failure_count = 0U;
  bool max_consecutive_drops_valid = true;

  void ObserveMapperFailureCount(uint32_t mapper_failure_count) noexcept {
    if (mapper_failure_count < previous_mapper_failure_count) {
      max_consecutive_drops_valid = false;
    } else {
      const uint32_t delta = mapper_failure_count - previous_mapper_failure_count;
      if (delta > max_consecutive_drops) {
        max_consecutive_drops = delta;
      }
    }
    previous_mapper_failure_count = mapper_failure_count;
    if (mapper_failure_count > timing_sample_drops) {
      timing_sample_drops = mapper_failure_count;
    }
  }

  void Observe(const icm42688_sample_t& sample) noexcept {
    if (sample.struct_size != sizeof(sample)) {
      ++invalid_samples;
      return;
    }

    ObserveMapperFailureCount(sample.mapper_failure_count);
    if (sample.timestamp_uncertainty_us > max_timestamp_uncertainty_us) {
      max_timestamp_uncertainty_us = sample.timestamp_uncertainty_us;
    }
    const uint64_t timestamp_ns = sample.sample_timestamp_ns;
    // 第一帧只建立时基，不产生采样间隔。
    if (samples == 0U) {
      first_timestamp_ns = timestamp_ns;
    } else if (timestamp_ns == last_timestamp_ns) {
      ++timestamp_duplicates;
    } else if (timestamp_ns < last_timestamp_ns) {
      ++timestamp_regressions;
    } else {
      // 只有前进的时间戳参与采样间隔统计。
      const uint64_t delta_ns = timestamp_ns - last_timestamp_ns;
      if (min_dt_ns == 0U || delta_ns < min_dt_ns) {
        min_dt_ns = delta_ns;
      }
      if (delta_ns > max_dt_ns) {
        max_dt_ns = delta_ns;
      }
      dt_sum_ns += delta_ns;
    }
    last_timestamp_ns = timestamp_ns;
    ++samples;
  }

  double EffectiveHz() const noexcept {
    if (samples < 2U || last_timestamp_ns <= first_timestamp_ns) {
      return 0.0;
    }
    return static_cast<double>(samples - 1U) * 1'000'000'000.0 /
           static_cast<double>(last_timestamp_ns - first_timestamp_ns);
  }
};

struct SensorDemoShutdownContext {
  robobaton_demo::SensorBagRecorder* recorder = nullptr;
  std::thread* imu_thread = nullptr;
  std::atomic<int>* imu_result = nullptr;
  ImuStats* imu_stats = nullptr;
  int* exit_code = nullptr;
  bool record_bag_requested = false;
  bool bag_finish_done = false;
  robobaton_demo::SensorBagFinishResult bag_finish;
  robobaton_demo::SensorBagRecorderStats bag_stats;
};

void StopImuAndFinishBagBeforeSc132Stop(const robobaton_demo::Sc132ShutdownResult& shutdown,
                                        void* user) noexcept {
  auto* context = static_cast<SensorDemoShutdownContext*>(user);
  if (context == nullptr) {
    return;
  }

  robobaton_demo::g_stop_requested.store(true, std::memory_order_release);
  if (context->imu_thread != nullptr && context->imu_thread->joinable()) {
    context->imu_thread->join();
  }
  if (context->imu_result != nullptr &&
      context->imu_result->load(std::memory_order_acquire) != 0 &&
      context->exit_code != nullptr) {
    *context->exit_code = 1;
  }
  if (context->imu_stats != nullptr && !context->imu_stats->max_consecutive_drops_valid &&
      context->exit_code != nullptr) {
    *context->exit_code = 1;
  }
  if (!context->record_bag_requested || context->recorder == nullptr ||
      context->bag_finish_done) {
    return;
  }

  // SC132 blocking stop 会等待所有 retained frame 归零；先停止 recorder 私有 worker，
  // 避免 JPEG staging slot 等待把底层相机 teardown 卡在 retained frame 上。
  const bool session_success = context->exit_code != nullptr && *context->exit_code == 0 &&
                               shutdown.consumer_join_ok && shutdown.ownership_quiescent &&
                               shutdown.rtsp_status_ok && shutdown.rtsp_close_ok;
  context->bag_finish = context->recorder->Finish(session_success);
  context->bag_stats = context->recorder->SnapshotStats();
  context->bag_finish_done = true;
}

struct SensorImuObserverState {
  ImuStats* stats = nullptr;
  robobaton_demo::ImuPrintState* print_state = nullptr;
  robobaton_demo::SensorBagRecorder* recorder = nullptr;
};

void ObserveSensorImuSample(const icm42688_sample_t& sample, void* user) {
  auto* state = static_cast<SensorImuObserverState*>(user);
  if (state == nullptr) {
    return;
  }
  if (state->stats != nullptr) {
    state->stats->Observe(sample);
  }
  if (state->print_state != nullptr) {
    robobaton_demo::PrintImuSample(sample, state->print_state);
  }
  if (state->recorder != nullptr) {
    state->recorder->ObserveImu(sample);
  }
}

}  // namespace

#ifdef RELEASE008_TESTING
namespace robobaton_demo {
SensorDemoImuStatsTestResult ObserveSensorDemoImuStatsForTest(
    const uint32_t* mapper_failure_counts, std::size_t count) {
  ImuStats stats;
  for (std::size_t index = 0; index < count; ++index) {
    icm42688_sample_t sample{};
    sample.struct_size = sizeof(sample);
    sample.sample_timestamp_ns = static_cast<uint64_t>(index + 1U) * 1'000'000ULL;
    sample.mapper_failure_count = mapper_failure_counts[index];
    stats.Observe(sample);
  }
  SensorDemoImuStatsTestResult result;
  result.timing_sample_drops = stats.timing_sample_drops;
  result.max_consecutive_drops = stats.max_consecutive_drops;
  result.max_consecutive_drops_valid = stats.max_consecutive_drops_valid;
  return result;
}
}  // namespace robobaton_demo
#endif

int main(int argc, char** argv) {
  using namespace robobaton_demo;

  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "sensor_demo " << ROBOBATON_RELEASE_VERSION << "\n"
              << "libicm42688 " << icm42688_get_version() << " abi="
              << ICM42688_ABI_VERSION_MAJOR << "." << ICM42688_ABI_VERSION_MINOR << "\n"
              << "libsc132 " << sc132_get_version() << " abi="
              << SC132_ABI_VERSION_MAJOR << "." << SC132_ABI_VERSION_MINOR << "\n"
              << "libprrtsp " << prrtsp_get_version() << " abi=2.0\n";
    return 0;
  }

  int exit_code = 0;
  bool sc_start_attempted = false;
  bool consumer_quiescent = false;
  bool rtsp_status_ok = true;
  bool rtsp_close_ok = true;
  bool rtsp_cleanup_done = false;
  bool rtsp_preview_complete = true;
  uint64_t rtsp_preview_dropped_total = 0U;
  uint64_t rtsp_preview_dropped_by_camera[kMaxChannels]{};
  int32_t rtsp_preview_last_error_by_camera[kMaxChannels]{};
  // 0 同时表示参数错误等硬件前失败路径尚未启动 IMU 线程。
  std::atomic<int> imu_result{0};
  ImuStats imu_stats;
  ImuPrintState imu_print_state;
  SensorImuObserverState imu_observer;
  ImuConsumerOptions imu_options;
  std::thread imu_thread;
  RtspChannels rtsp;
  std::unique_ptr<FramePipeline> pipeline;
  std::unique_ptr<FrozenSystemClock> system_clock;
  std::unique_ptr<ScopedNonblockingFd> output_mode;
  uint32_t imu_sample_drop_policy = ICM42688_SAMPLE_DROP_POLICY_ALLOW_COUNTED;
  SensorBagRecorder recorder;
  bool record_bag_requested = false;
  std::string record_bag_path;
  SensorDemoShutdownContext shutdown_context;
  DiskObservabilitySnapshot disk_observability_start;

  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);
    g_stop_requested.store(false, std::memory_order_release);

    Options options = ParseSensorDemoCommandLine(argc, argv);
    imu_sample_drop_policy = options.imu_sample_drop_policy;
    system_clock = std::make_unique<FrozenSystemClock>();
    system_clock->PrintTimeBase(std::cout);
    options.system_clock = system_clock.get();
    record_bag_requested = !options.record_bag_path.empty();
    if (record_bag_requested) {
      disk_observability_start = CaptureDiskObservability();
    }
    record_bag_path = options.record_bag_path;
    if (record_bag_requested) {
      options.rtsp_preview_failure_policy = RtspPreviewFailurePolicy::kDegradePreview;
      recorder.Start(options, record_bag_path);
    }
    imu_options.sample_rate_hz = options.imu_sample_rate_hz;
    imu_options.sample_drop_policy = options.imu_sample_drop_policy;
    imu_options.count = 0U;
    imu_options.stop_requested = &g_stop_requested;
    imu_options.system_clock = system_clock.get();
    imu_print_state.print_every_samples =
        ImuPrintEverySamples(options.imu_sample_rate_hz, options.imu_print_rate_hz);
    imu_print_state.print_metrics = options.imu_print_metrics;
    output_mode = std::make_unique<ScopedNonblockingFd>(imu_print_state.output_fd);
    imu_print_state.output_available = output_mode->active();
    imu_observer.stats = &imu_stats;
    imu_observer.print_state = &imu_print_state;
    imu_observer.recorder = record_bag_requested ? &recorder : nullptr;
    std::cout << "Starting sensor_demo channels=" << options.channels
              << " camera_mask=0x" << std::hex << options.camera_mask << std::dec
              << " output_size=" << OutputWidth(options) << "x" << OutputHeight(options)
              << " fps=" << options.fps << " rotate=" << options.rotate_degrees
              << " kbps=" << options.bps << " codec=" << VideoCodecName(options.video_codec)
              << " rtsp_base_port=" << options.rtsp_base_port
              << " path=" << options.url << " imu_rate_hz=" << options.imu_sample_rate_hz
              << " imu_sample_drop_policy="
              << ImuSampleDropPolicyName(options.imu_sample_drop_policy)
              << " imu_start_order="
              << (options.imu_start_order == ImuStartOrder::kCameraFirst
                      ? "camera-first"
                      : "imu-first")
              << " imu_print_rate_hz=" << options.imu_print_rate_hz
              << " imu_print_metrics=" << (options.imu_print_metrics ? "on" : "off")
              << " imu_time=SENSOR_TIMESTAMP_FIFO record_bag="
              << (record_bag_requested ? record_bag_path : "disabled")
              << " rtsp_preview_policy="
              << (options.rtsp_preview_failure_policy ==
                          RtspPreviewFailurePolicy::kDegradePreview
                      ? "degrade-preview"
                      : "fail-closed");
    if (record_bag_requested) {
      std::cout << " record_frame_skip=" << options.record_frame_skip
                << " image_persistence_fps="
                << static_cast<uint32_t>(options.fps) /
                       (options.record_frame_skip + 1U);
    }
    std::cout << "\n" << std::flush;

    // 两种顺序复用同一线程入口；imu_options 和 observer 状态的生命周期覆盖统一清理和 join。
    const auto start_imu = [&imu_result, &imu_options, &imu_observer, &imu_thread] {
      imu_thread = std::thread([&imu_result, &imu_options, &imu_observer] {
        const int result = RunIcmConsumer(imu_options, ObserveSensorImuSample, &imu_observer);
        imu_result.store(result, std::memory_order_release);
        if (result != 0) {
          g_stop_requested.store(true, std::memory_order_release);
        }
      });
    };

    // 显式 imu-first 回滚路径在任何相机配置副作用之前启动 IMU。
    if (options.imu_start_order == ImuStartOrder::kImuFirst) {
      start_imu();
    }

    if (sc132_set_fps(static_cast<uint32_t>(options.fps)) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_fps failed");
    }
    if (sc132_set_output_rotation(
            static_cast<uint32_t>(InternalRotateDegrees(options))) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_output_rotation failed");
    }
    ConfigureSc132TriggerMode(options);
    ConfigureSc132SensorProfile(options);

    pipeline = std::make_unique<FramePipeline>(
        options, &rtsp, MainPipelineHooks(record_bag_requested ? &recorder : nullptr));
    pipeline->StartWorkers();

    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options.camera_mask, camera_id)) {
        continue;
      }
      const int32_t status =
          rtsp.Open(camera_id, RtspPortForChannel(options, camera_id), options);
      if (status != PRRTSP_OK) {
        throw std::runtime_error("prrtsp_stream_open failed for camera " +
                                 std::to_string(camera_id) + " status=" +
                                 std::to_string(status));
      }
    }

    pipeline->StartRuntimeMonitor();
    sc132_frame_set_config_t config = pipeline->MakeFrameSetConfig();
    sc_start_attempted = true;
    const int32_t start_status = sc132_start_frame_set(&config, options.camera_mask);
    if (start_status != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_start_frame_set failed status=" +
                               std::to_string(start_status));
    }
    pipeline->MarkSourceStarted();

    // camera-first 只在所选 frame-set 启动成功后建立 IMU final producer epoch。
    if (options.imu_start_order == ImuStartOrder::kCameraFirst) {
      start_imu();
    }

    while (g_signal_stop == 0 &&
           !g_stop_requested.load(std::memory_order_acquire) &&
           pipeline->FirstError() == 0 && !recorder.HasFatalError()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << "\n";
    exit_code = 1;
    g_stop_requested.store(true, std::memory_order_release);
  } catch (...) {
    std::cerr << "fatal: unknown C++ exception\n";
    exit_code = 1;
    g_stop_requested.store(true, std::memory_order_release);
  }

  if (pipeline != nullptr) {
    if (sc_start_attempted) {
      if (pipeline->FirstError() != 0 || recorder.HasFatalError()) {
        exit_code = 1;
      }
      shutdown_context.recorder = &recorder;
      shutdown_context.imu_thread = &imu_thread;
      shutdown_context.imu_result = &imu_result;
      shutdown_context.imu_stats = &imu_stats;
      shutdown_context.exit_code = &exit_code;
      shutdown_context.record_bag_requested = record_bag_requested;
      const Sc132ShutdownResult shutdown = FinishSc132ShutdownDetailed(
          pipeline.get(), &rtsp,
          record_bag_requested ? StopImuAndFinishBagBeforeSc132Stop : nullptr,
          record_bag_requested ? &shutdown_context : nullptr);
      consumer_quiescent = shutdown.consumer_join_ok && shutdown.ownership_quiescent;
      rtsp_status_ok = shutdown.rtsp_status_ok;
      rtsp_close_ok = shutdown.rtsp_close_ok;
      rtsp_cleanup_done = shutdown.sc132_cleanup_reached;
    } else {
      pipeline->BeginShutdown(false);
      consumer_quiescent = pipeline->Join();
    }
    rtsp_preview_complete = pipeline->RtspPreviewComplete();
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      rtsp_preview_dropped_by_camera[camera_id] =
          pipeline->RtspPreviewDroppedFrames(camera_id);
      rtsp_preview_last_error_by_camera[camera_id] =
          pipeline->RtspPreviewLastError(camera_id);
      rtsp_preview_dropped_total += rtsp_preview_dropped_by_camera[camera_id];
    }
    if (pipeline->FirstError() != 0) {
      std::cerr << "fatal: pipeline first_error=" << pipeline->FirstError() << "\n";
      exit_code = 1;
    }
  } else {
    consumer_quiescent = true;
  }

  if (!consumer_quiescent) {
    std::cerr << "fatal: consumer join failed; skipping RTSP status/close\n" << std::flush;
    std::_Exit(1);
  }

  if (!rtsp_cleanup_done) {
    rtsp_status_ok = rtsp.CaptureStatuses();
    rtsp_close_ok = rtsp.CloseReverse();
  }
  if (!rtsp_status_ok) {
    std::cerr << "fatal: prrtsp_stream_get_status failed\n";
    exit_code = 1;
  }
  if (!rtsp_close_ok) {
    std::cerr << "fatal: RTSP handle remains after three close attempts\n";
    exit_code = 1;
  }
  if (!rtsp_preview_complete && !record_bag_requested) {
    std::cerr << "fatal: RTSP preview degraded without record-bag isolation\n";
    exit_code = 1;
  }

  // Camera/RTSP are quiescent before stopping the independent IMU producer.
  g_stop_requested.store(true, std::memory_order_release);
  if (imu_thread.joinable()) {
    imu_thread.join();
  }
  // IMU 高速终端输出已结束，最终摘要恢复普通 stdout flags，便于脚本稳定读取。
  output_mode.reset();
  if (imu_result.load(std::memory_order_acquire) != 0) {
    std::cerr << "fatal: IMU INT1 producer failed\n";
    exit_code = 1;
  }
  if (!imu_stats.max_consecutive_drops_valid) {
    std::cerr
        << "fatal: IMU mapper_failure_count regressed; max_consecutive_drops evidence invalid\n";
    exit_code = 1;
  }

  std::cout << "SENSOR_RTSP_RESULT preview_complete="
            << (rtsp_preview_complete ? "yes" : "no")
            << " preview_dropped_total=" << rtsp_preview_dropped_total
            << " preview_dropped_by_camera=cam0:" << rtsp_preview_dropped_by_camera[0]
            << ",cam1:" << rtsp_preview_dropped_by_camera[1]
            << ",cam2:" << rtsp_preview_dropped_by_camera[2]
            << ",cam3:" << rtsp_preview_dropped_by_camera[3]
            << " preview_last_error_by_camera=cam0:"
            << rtsp_preview_last_error_by_camera[0]
            << ",cam1:" << rtsp_preview_last_error_by_camera[1]
            << ",cam2:" << rtsp_preview_last_error_by_camera[2]
            << ",cam3:" << rtsp_preview_last_error_by_camera[3] << "\n";

  if (record_bag_requested) {
    const SensorBagFinishResult bag_finish = shutdown_context.bag_finish_done
                                                ? shutdown_context.bag_finish
                                                : recorder.Finish(exit_code == 0);
    const SensorBagRecorderStats bag_stats = shutdown_context.bag_finish_done
                                                ? shutdown_context.bag_stats
                                                : recorder.SnapshotStats();
    const DiskObservabilitySnapshot disk_observability_finish =
        CaptureDiskObservability();
    if (!bag_finish || recorder.HasFatalError()) {
      const std::string error = !bag_finish.error.empty() ? bag_finish.error
                                                          : recorder.ErrorMessage();
      std::cerr << "fatal: bag recorder failed: " << error << "\n";
      exit_code = 1;
    }
    const uint64_t image_span_ns =
        bag_stats.has_image_timestamp &&
                bag_stats.last_image_timestamp_ns > bag_stats.first_image_timestamp_ns
            ? bag_stats.last_image_timestamp_ns - bag_stats.first_image_timestamp_ns
            : 0U;
    const std::string published_path = bag_finish.published_path.empty()
                                           ? record_bag_path
                                           : bag_finish.published_path;
    std::cout << "SENSOR_BAG_RESULT path=" << TerminalToken(published_path)
              << " configured_path=" << TerminalToken(record_bag_path)
              << " bag_outcome="
              << SensorBagFinishOutcomeName(bag_finish.outcome)
              << " data_complete=" << (bag_finish.data_complete ? "yes" : "no")
              << " cleanup_complete=" << (bag_finish.cleanup_complete ? "yes" : "no")
              << " session_uuid=" << TerminalToken(bag_finish.session_uuid)
              << " quarantine_path=" << TerminalToken(bag_finish.quarantine_path)
              << " image_frames=" << bag_stats.image_frames
              << " image_frames_by_camera=cam0:" << bag_stats.image_frames_by_camera[0]
              << ",cam1:" << bag_stats.image_frames_by_camera[1]
              << ",cam2:" << bag_stats.image_frames_by_camera[2]
              << ",cam3:" << bag_stats.image_frames_by_camera[3]
              << " rtsp_preview_complete="
              << (rtsp_preview_complete ? "yes" : "no")
              << " rtsp_preview_dropped_total=" << rtsp_preview_dropped_total
              << " image_first_ns=" << bag_stats.first_image_timestamp_ns
              << " image_last_ns=" << bag_stats.last_image_timestamp_ns
              << " image_span_ns=" << image_span_ns
              << " source_frame_sets_seen=" << bag_stats.source_frame_sets_seen
              << " recorder_selected_groups=" << bag_stats.recorder_selected_groups
              << " recorder_admitted_groups=" << bag_stats.recorder_admitted_groups
              << " recorder_dropped_groups=" << bag_stats.recorder_dropped_groups
              << " recorder_written_groups=" << bag_stats.recorder_written_groups
              << " recorder_imu_admitted=" << bag_stats.recorder_imu_admitted
              << " recorder_imu_written=" << bag_stats.recorder_imu_written
              << " imu_samples=" << bag_stats.imu_samples
              << " frame_queue_capacity=" << bag_stats.frame_queue_capacity
              << " frame_queue_high_watermark=" << bag_stats.frame_queue_high_watermark
              << " frame_queue_peak_depth=" << bag_stats.frame_queue_peak_depth
              << " frame_queue_at_capacity_dwell_ns="
              << bag_stats.frame_queue_at_capacity_dwell_ns
              << " frame_queue_at_capacity_events="
              << bag_stats.frame_queue_at_capacity_events
              << " frame_queue_full_rejects=" << bag_stats.frame_queue_full_rejects
              << " encoded_queue_capacity=" << bag_stats.encoded_queue_capacity
              << " encoded_queue_high_watermark="
              << bag_stats.encoded_queue_high_watermark
              << " encoded_queue_peak_depth=" << bag_stats.encoded_queue_peak_depth
              << " encoded_queue_at_capacity_dwell_ns="
              << bag_stats.encoded_queue_at_capacity_dwell_ns
              << " encoded_queue_at_capacity_events="
              << bag_stats.encoded_queue_at_capacity_events
              << " encoded_queue_full_rejects="
              << bag_stats.encoded_queue_full_rejects
              << " encoded_queue_oldest_evicted_groups="
              << bag_stats.encoded_queue_oldest_evicted_groups
              << " encoded_queue_oldest_evicted_frames="
              << bag_stats.encoded_queue_oldest_evicted_frames
              << " encoded_queue_oldest_evicted_bytes="
              << bag_stats.encoded_queue_oldest_evicted_bytes
              << " imu_queue_capacity=" << bag_stats.imu_queue_capacity
              << " imu_queue_high_watermark=" << bag_stats.imu_queue_high_watermark
              << " imu_queue_peak_depth=" << bag_stats.imu_queue_peak_depth
              << " imu_queue_at_capacity_dwell_ns="
              << bag_stats.imu_queue_at_capacity_dwell_ns
              << " imu_queue_at_capacity_events="
              << bag_stats.imu_queue_at_capacity_events
              << " imu_queue_full_rejects=" << bag_stats.imu_queue_full_rejects
              << " writer_image_backlog_peak_depth=" << bag_stats.writer_image_backlog_peak_depth
              << " frame_metadata_messages=" << bag_stats.frame_metadata_messages
              << " jpeg_bytes=" << bag_stats.jpeg_bytes
              << " nv12_copy_bytes=" << bag_stats.nv12_copy_bytes
              << " nv12_copy_bulk_plane_count="
              << bag_stats.nv12_copy_bulk_plane_count
              << " nv12_copy_bulk_bytes=" << bag_stats.nv12_copy_bulk_bytes
              << " nv12_copy_row_count=" << bag_stats.nv12_copy_row_count
              << " nv12_copy_row_bytes=" << bag_stats.nv12_copy_row_bytes
              << " write_order_wait_max_ns=" << bag_stats.write_order_wait_max_ns
              << " writer_mutex_wait_max_ns=" << bag_stats.writer_mutex_wait_max_ns
              << " writer_mutex_hold_max_ns=" << bag_stats.writer_mutex_hold_max_ns;
    AppendBagLatencySummary(std::cout, "nv12_copy", bag_stats.nv12_copy_latency);
    AppendBagLatencySummary(std::cout, "jpeg_encode", bag_stats.jpeg_encode_latency);
    AppendBagLatencySummary(std::cout, "write_order_wait", bag_stats.write_order_wait_latency);
    AppendBagLatencySummary(std::cout, "image_writer_wait", bag_stats.image_writer_wait_latency);
    AppendBagLatencySummary(std::cout, "image_writer_hold", bag_stats.image_writer_hold_latency);
    AppendBagLatencySummary(std::cout, "imu_writer_wait", bag_stats.imu_writer_wait_latency);
    AppendBagLatencySummary(std::cout, "imu_writer_hold", bag_stats.imu_writer_hold_latency);
    AppendRosbagWriterSummary(std::cout, bag_stats.writer_stats);
    AppendDiskObservabilitySummary(std::cout, disk_observability_start,
                                   disk_observability_finish);
    std::cout << " success=" << (exit_code == 0 ? "yes" : "no") << "\n";
  }

  std::cout << "SENSOR_IMU_RESULT samples=" << imu_stats.samples
            << " invalid=" << imu_stats.invalid_samples
            << " timestamp_duplicates=" << imu_stats.timestamp_duplicates
            << " timestamp_regressions=" << imu_stats.timestamp_regressions
            << " effective_hz=" << imu_stats.EffectiveHz()
            << " min_dt_ns=" << imu_stats.min_dt_ns
            << " max_dt_ns=" << imu_stats.max_dt_ns
            << " timing_sample_drops=" << imu_stats.timing_sample_drops
            << " max_uncertainty_us=" << imu_stats.max_timestamp_uncertainty_us
            << " max_consecutive_drops=" << imu_stats.max_consecutive_drops
            << " imu_sample_drop_policy="
            << ImuSampleDropPolicyName(imu_sample_drop_policy) << "\n";
  std::cout << "sensor_demo stopped exit_code=" << exit_code << "\n";
  return exit_code;
}
