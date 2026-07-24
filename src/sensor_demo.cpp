#include <signal.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

extern "C" {
#include "icm42688_driver.h"
#include "sc132camera.h"
}

#include "cam_demo_common.h"
#include "cam_demo_config.h"
#include "cam_demo_pipeline.h"
#include "cam_demo_rtsp.h"

namespace {

volatile sig_atomic_t g_signal_stop = 0;

void SignalHandler(int) {
  g_signal_stop = 1;
}

#ifdef RELEASE008_TESTING
bool InjectJoinFailure(std::thread&, void*) { return false; }
#endif

robobaton_demo::PipelineHooks MainPipelineHooks() {
  robobaton_demo::PipelineHooks hooks{};
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

  void Observe(const icm42688_sample_t& sample) noexcept {
    if (sample.struct_size != sizeof(sample)) {
      ++invalid_samples;
      return;
    }

    const uint64_t timestamp_ns = sample.host_timestamp_ns;
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

void ObserveImuSample(const icm42688_sample_t& sample, void* user) {
  auto* stats = static_cast<ImuStats*>(user);
  if (stats != nullptr) {
    stats->Observe(sample);
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace robobaton_demo;

  int exit_code = 0;
  bool sc_start_attempted = false;
  bool consumer_quiescent = false;
  std::atomic<int> imu_result{-1};
  ImuStats imu_stats;
  std::thread imu_thread;
  auto* rtsp = new RtspChannels();
  FramePipeline* pipeline = nullptr;

  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    g_stop_requested.store(false, std::memory_order_release);

    const Options options = ParseCommandLine(argc, argv);
    std::cout << "Starting sensor_demo channels=" << options.channels
              << " camera_mask=0x" << std::hex << options.camera_mask << std::dec
              << " output_size=" << OutputWidth(options) << "x" << OutputHeight(options)
              << " fps=" << options.fps << " rotate=" << options.rotate_degrees
              << " kbps=" << options.bps << " codec=" << VideoCodecName(options.video_codec)
              << " path=" << options.url << " imu_rate_hz=1000 read_mode=INT1_DIRECT\n";

    // IMU starts first so its direct INT1 timeline is live before camera frames arrive.
    ImuConsumerOptions imu_options;
    imu_options.sample_rate_hz = 1000U;
    imu_options.count = 0U;
    imu_options.stop_requested = &g_stop_requested;
    imu_thread = std::thread([&imu_result, &imu_options, &imu_stats] {
      const int result = RunIcmConsumer(imu_options, ObserveImuSample, &imu_stats);
      imu_result.store(result, std::memory_order_release);
      if (result != 0) {
        g_stop_requested.store(true, std::memory_order_release);
      }
    });

    if (sc132_set_fps(static_cast<uint32_t>(options.fps)) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_fps failed");
    }
    if (sc132_set_output_rotation(
            static_cast<uint32_t>(InternalRotateDegrees(options))) != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_set_output_rotation failed");
    }
    ConfigureSc132TriggerMode(options);
    ConfigureSc132SensorProfile(options);

    pipeline = new FramePipeline(options, rtsp, MainPipelineHooks());
    pipeline->StartWorkers();

    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraMaskContains(options.camera_mask, camera_id)) {
        continue;
      }
      const int32_t status =
          rtsp->Open(camera_id, RtspPortForChannel(camera_id), options);
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

    while (g_signal_stop == 0 &&
           !g_stop_requested.load(std::memory_order_acquire) &&
           pipeline->FirstError() == 0) {
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
      consumer_quiescent = FinishSc132Shutdown(pipeline);
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

  if (!rtsp->CaptureStatuses()) {
    std::cerr << "fatal: prrtsp_stream_get_status failed\n";
    exit_code = 1;
  }
  if (!rtsp->CloseReverse()) {
    std::cerr << "fatal: RTSP handle remains after three close attempts\n";
    exit_code = 1;
  }

  // Camera/RTSP are quiescent before stopping the independent IMU producer.
  g_stop_requested.store(true, std::memory_order_release);
  if (imu_thread.joinable()) {
    imu_thread.join();
  }
  if (imu_result.load(std::memory_order_acquire) != 0) {
    std::cerr << "fatal: IMU INT1 producer failed\n";
    exit_code = 1;
  }

  std::cout << "SENSOR_IMU_RESULT samples=" << imu_stats.samples
            << " invalid=" << imu_stats.invalid_samples
            << " timestamp_duplicates=" << imu_stats.timestamp_duplicates
            << " timestamp_regressions=" << imu_stats.timestamp_regressions
            << " effective_hz=" << imu_stats.EffectiveHz()
            << " min_dt_ns=" << imu_stats.min_dt_ns
            << " max_dt_ns=" << imu_stats.max_dt_ns << "\n";
  std::cout << "sensor_demo stopped exit_code=" << exit_code << "\n";
  return exit_code;
}
