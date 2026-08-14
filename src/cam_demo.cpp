#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <thread>

extern "C" {
#include "prrtsp_v2.h"
#include "sc132camera.h"
}

#include "cam_demo_common.h"
#include "cam_demo_config.h"
#include "cam_demo_pipeline.h"
#include "cam_demo_rtsp.h"

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

namespace {

volatile sig_atomic_t g_signal_stop = 0;

void SignalHandler(int) { g_signal_stop = 1; }

#ifdef RELEASE008_TESTING
bool InjectJoinFailure(std::thread&, void*) { return false; }
#endif

robobaton_demo::PipelineHooks MainPipelineHooks() {
  robobaton_demo::PipelineHooks hooks{};
#ifdef RELEASE008_TESTING
  // Host production-bound 测试可在真实 main cleanup owner 中注入 join failure；
  // release build 不包含该环境注入分支。
  const char* inject = std::getenv("RELEASE008_TEST_JOIN_FAILURE");
  if (inject != nullptr && inject[0] != '\0') {
    hooks.join_thread = InjectJoinFailure;
  }
#endif
  return hooks;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace robobaton_demo;

  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "cam_demo " << ROBOBATON_RELEASE_VERSION << "\n"
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

  // callback context 与关闭失败后仍被 producer 持有的 handle
  // 必须存活到进程结束，避免析构触发 restart/unload。
  auto* rtsp = new RtspChannels();
  FramePipeline* pipeline = nullptr;
  std::unique_ptr<FrozenSystemClock> system_clock;

  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    g_stop_requested.store(false, std::memory_order_release);

    Options options = ParseCommandLine(argc, argv);
    system_clock = std::make_unique<FrozenSystemClock>();
    system_clock->PrintTimeBase(std::cout);
    options.system_clock = system_clock.get();
    std::cout << "Starting SC132 v2 RTSP demo channels=" << options.channels
              << " camera_mask=0x" << std::hex << options.camera_mask << std::dec
              << " output_size=" << OutputWidth(options) << "x" << OutputHeight(options)
              << " fps=" << options.fps << " rotate=" << options.rotate_degrees
              << " kbps=" << options.bps << " codec=" << VideoCodecName(options.video_codec)
              << " path=" << options.url << "\n";

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
          rtsp->Open(camera_id, RtspPortForChannel(options, camera_id), options);
      if (status != PRRTSP_OK) {
        throw std::runtime_error("prrtsp_stream_open failed for camera " +
                                 std::to_string(camera_id) + " status=" +
                                 std::to_string(status));
      }
    }

    pipeline->StartRuntimeMonitor();
    sc132_frame_set_config_t config = pipeline->MakeFrameSetConfig();
    // start 可能部分创建 producer threads；attempt 必须先锁存，失败也执行 drain/join/stop×2。
    sc_start_attempted = true;
    const int32_t start_status = sc132_start_frame_set(&config, options.camera_mask);
    if (start_status != SC132_STATUS_OK) {
      throw std::runtime_error("sc132_start_frame_set failed status=" +
                               std::to_string(start_status));
    }
    pipeline->MarkSourceStarted();

    while (g_signal_stop == 0 && !g_stop_requested.load(std::memory_order_acquire) &&
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
      const Sc132ShutdownResult shutdown = FinishSc132ShutdownDetailed(pipeline, rtsp);
      consumer_quiescent = shutdown.consumer_join_ok && shutdown.ownership_quiescent;
      rtsp_status_ok = shutdown.rtsp_status_ok;
      rtsp_close_ok = shutdown.rtsp_close_ok;
      rtsp_cleanup_done = shutdown.sc132_cleanup_reached;
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
    // join failure 后尚未 quiescent，必须保留 ownership 并非零终止。
    std::cerr << "fatal: consumer join failed; skipping RTSP status/close\n" << std::flush;
    std::_Exit(1);
  }

  if (!rtsp_cleanup_done) {
    rtsp_status_ok = rtsp->CaptureStatuses();
    rtsp_close_ok = rtsp->CloseReverse();
  }
  if (!rtsp_status_ok) {
    std::cerr << "fatal: prrtsp_stream_get_status failed\n";
    exit_code = 1;
  }
  if (!rtsp_close_ok) {
    std::cerr << "fatal: RTSP handle remains after three close attempts\n";
    exit_code = 1;
  }

  std::cout << "SC132 v2 RTSP demo stopped exit_code=" << exit_code << "\n";
  return exit_code;
}
