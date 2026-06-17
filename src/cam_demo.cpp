#include <signal.h>

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" {
#include "hb_mem_mgr.h"
#include "sc132camera.h"
}

#include "cam_demo_common.h"
#include "cam_demo_config.h"
#include "cam_demo_pipeline.h"
#include "cam_demo_rtsp.h"

namespace robobaton_demo {

// 功能：处理 SIGINT/SIGTERM，通知主循环和后台线程退出。
// 输入：signum 由系统信号传入，本 demo 不区分具体信号。
// 输出：无。
// 副作用：设置全局停止标志 g_stop_requested。
void SignalHandler(int /*signum*/) {
  g_stop_requested.store(true);
}

// 功能：用户二次开发的单帧入口。
// 输入：frame 已 retain，包含 NV12 图像、DMA 地址、相机时间戳和组序号。
// 输出：无。
// 生命周期：函数返回后 demo 会继续推流并释放 frame；若要异步使用图像，请复制 Y/UV 数据或自行 retain。
void OnQueuedCameraFrame(const QueuedFrame& frame) {
  (void)frame;
}

// 功能：用户二次开发的四目同步帧组入口。
// 输入：frame_set 已由 libsc132 配组，同一 group_id 下四路 frame_id 对外完全一致。
// 输出：无。
// 生命周期：frame_set 内的 frame 只在回调期间可靠；异步保存时需要自行 retain/release。
void OnSynchronizedFrameSet(const sc132_frame_set_t& frame_set) {
  (void)frame_set;
}

}  // namespace robobaton_demo

// 功能：启动四目相机采集、libsc132 帧组同步、RTSP 推流和诊断流程。
// 输入：argc/argv 为命令行参数。
// 输出：0 表示正常退出，非 0 表示初始化或运行阶段失败。
// 副作用：打开 hbmem、初始化 RTSP 和相机链路，退出时按顺序释放资源。
int main(int argc, char** argv) {
  using namespace robobaton_demo;

  int exit_code = 0;
  int initialized_rtsp_channels = 0;
  bool hb_mem_opened = false;
  bool camera_started = false;
  std::array<RtspEndpoint, kMaxChannels> initialized_rtsp_endpoints{};

  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    const Options options = ParseCommandLine(argc, argv);
    std::cout << "Starting SC132 4-camera RTSP demo"
              << " channels=" << options.channels
              << " input_size=" << options.width << "x" << options.height
              << " output_size=" << OutputWidth(options) << "x" << OutputHeight(options)
              << " fps=" << options.fps
              << " bps=" << options.bps
              << " url=" << options.url
              << " trigger_mode=" << options.trigger_mode
              << " frame_set_max_skew_ns=" << options.frame_set_max_skew_ns
              << " frame_set_timeout_ms=" << options.frame_set_timeout_ms
              << " diagnostics=" << (options.diagnostics ? "on" : "off") << "\n";

    if (VioCamSetFps(options.fps) != 0) {
      throw std::runtime_error("VioCamSetFps failed");
    }
    if (VioCamSetOutputRotate(options.rotate_degrees) != 0) {
      throw std::runtime_error("VioCamSetOutputRotate failed");
    }
    ConfigureSc132TriggerMode(options);
    ConfigureSc132SensorProfile(options);

    // X5 上相机和编码链路依赖 hbmem 管理 DMA buffer，必须先打开模块。
    if (hb_mem_module_open() != 0) {
      throw std::runtime_error("hb_mem_module_open failed");
    }
    hb_mem_opened = true;

    FramePipeline pipeline(options);
    pipeline.StartWorkers();

    for (int channel = 0; channel < options.channels; ++channel) {
      const RtspEndpoint endpoint = RtspEndpointForChannel(channel);
      const int port = RtspPortForChannel(channel);
      if (InitRtspEndpoint(endpoint, port, options) != 0) {
        throw std::runtime_error("init RTSP channel " + std::to_string(channel) + " failed");
      }
      initialized_rtsp_endpoints[channel] = endpoint;
      ++initialized_rtsp_channels;
    }

    pipeline.StartDiagnosticsIfEnabled();

    // 使用 libsc132 帧组 API：只有配组后的同步帧才进入用户 hook 和 RTSP 队列。
    sc132_frame_set_config_t frame_set_config = pipeline.MakeFrameSetConfig();
    if (VioCamInitmFrameSet(&frame_set_config) != 0) {
      throw std::runtime_error("VioCamInitmFrameSet failed");
    }
    camera_started = true;

    while (!g_stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (camera_started) {
      VioCamClose();
      camera_started = false;
    }
    pipeline.Stop();
    pipeline.Join();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    exit_code = 1;
    g_stop_requested.store(true);
  }

  if (camera_started) {
    VioCamClose();
  }

  CloseRtspChannels(initialized_rtsp_endpoints, initialized_rtsp_channels);
  if (hb_mem_opened) {
    // 等相机帧和 RTSP 通道都释放后关闭 hbmem，避免 DMA buffer 仍被使用。
    (void)hb_mem_module_close();
  }

  std::cout << "SC132 4-camera RTSP demo stopped\n";
  return exit_code;
}
