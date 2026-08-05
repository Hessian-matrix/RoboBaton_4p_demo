#include <signal.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
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

void ObserveQueuedFrameForBag(const robobaton_demo::QueuedFrame& frame, void* user) {
  auto* recorder = static_cast<robobaton_demo::SensorBagRecorder*>(user);
  if (recorder != nullptr) {
    recorder->ObserveFrame(frame);
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

robobaton_demo::PipelineHooks MainPipelineHooks(
    robobaton_demo::SensorBagRecorder* recorder) {
  robobaton_demo::PipelineHooks hooks{};
  if (recorder != nullptr) {
    hooks.on_queued_frame = ObserveQueuedFrameForBag;
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
    record_bag_path = options.record_bag_path;
    if (record_bag_requested) {
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
              << (record_bag_requested ? record_bag_path : "disabled");
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

    pipeline->StartDiagnosticsIfEnabled();
    sc132_frame_set_config_t config = pipeline->MakeFrameSetConfig();
    sc_start_attempted = true;
    const int32_t start_status = sc132_start_frame_set(&config, options.camera_mask);
    if (start_status != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_start_frame_set failed status=" +
                               std::to_string(start_status));
    }

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
      consumer_quiescent = FinishSc132Shutdown(pipeline.get(), &rtsp);
    } else {
      pipeline->BeginShutdown(false);
      consumer_quiescent = pipeline->Join();
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

  if (!rtsp.CaptureStatuses()) {
    std::cerr << "fatal: prrtsp_stream_get_status failed\n";
    exit_code = 1;
  }
  if (!rtsp.CloseReverse()) {
    std::cerr << "fatal: RTSP handle remains after three close attempts\n";
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

  if (record_bag_requested) {
    const bool recorder_ok = recorder.Finish(exit_code == 0);
    const SensorBagRecorderStats bag_stats = recorder.SnapshotStats();
    if (!recorder_ok || recorder.HasFatalError()) {
      std::cerr << "fatal: bag recorder failed: " << recorder.ErrorMessage() << "\n";
      exit_code = 1;
    }
    const uint64_t image_span_ns =
        bag_stats.has_image_timestamp &&
                bag_stats.last_image_timestamp_ns > bag_stats.first_image_timestamp_ns
            ? bag_stats.last_image_timestamp_ns - bag_stats.first_image_timestamp_ns
            : 0U;
    std::cout << "SENSOR_BAG_RESULT path=" << record_bag_path
              << " image_frames=" << bag_stats.image_frames
              << " image_frames_by_camera=cam0:" << bag_stats.image_frames_by_camera[0]
              << ",cam1:" << bag_stats.image_frames_by_camera[1]
              << ",cam2:" << bag_stats.image_frames_by_camera[2]
              << ",cam3:" << bag_stats.image_frames_by_camera[3]
              << " image_first_ns=" << bag_stats.first_image_timestamp_ns
              << " image_last_ns=" << bag_stats.last_image_timestamp_ns
              << " image_span_ns=" << image_span_ns
              << " imu_samples=" << bag_stats.imu_samples
              << " imu_queue_peak_depth=" << bag_stats.imu_queue_peak_depth
              << " imu_queue_full_rejects=" << bag_stats.imu_queue_full_rejects
              << " frame_queue_peak_depth=" << bag_stats.frame_queue_peak_depth
              << " frame_queue_full_rejects=" << bag_stats.frame_queue_full_rejects
              << " encoded_queue_peak_depth=" << bag_stats.encoded_queue_peak_depth
              << " encoded_queue_full_rejects=" << bag_stats.encoded_queue_full_rejects
              << " frame_metadata_messages=" << bag_stats.frame_metadata_messages
              << " jpeg_bytes=" << bag_stats.jpeg_bytes
              << " nv12_copy_bytes=" << bag_stats.nv12_copy_bytes
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
