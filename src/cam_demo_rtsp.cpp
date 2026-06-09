#include "cam_demo_rtsp.h"

extern "C" {
#include "pr_venc.h"
}

namespace robobaton_demo {

// 功能：调用 libprrtsp 对应通道的 NV12 送帧接口。
// 输入：endpoint 指定 ch1..ch4；frame 提供 Y/UV 地址、物理地址和 RTSP PTS。
// 输出：无。
// 副作用：图像被送入编码器和 RTSP session；frame 的引用仍由调用方释放。
void SendFrameToRtspEndpoint(RtspEndpoint endpoint, const QueuedFrame& frame) {
  switch (endpoint) {
    case RtspEndpoint::kCh1:
      Rtsp_SendImg_ch1_planes(frame.y_data, frame.uv_data, frame.y_phys, frame.uv_phys, 0,
                              frame.rtsp_timestamp_ns);
      break;
    case RtspEndpoint::kCh2:
      Rtsp_SendImg_ch2_planes(frame.y_data, frame.uv_data, frame.y_phys, frame.uv_phys, 0,
                              frame.rtsp_timestamp_ns);
      break;
    case RtspEndpoint::kCh3:
      Rtsp_SendImg_ch3_planes(frame.y_data, frame.uv_data, frame.y_phys, frame.uv_phys, 0,
                              frame.rtsp_timestamp_ns);
      break;
    case RtspEndpoint::kCh4:
      Rtsp_SendImg_ch4_planes(frame.y_data, frame.uv_data, frame.y_phys, frame.uv_phys, 0,
                              frame.rtsp_timestamp_ns);
      break;
    default:
      break;
  }
}

// 功能：初始化一路 RTSP 编码器和 RTSP 服务。
// 输入：endpoint 指定通道，port 指定监听端口，options 提供输出尺寸、fps、码率和 URL。
// 输出：0 表示成功，非 0 表示初始化失败。
// 副作用：在 libprrtsp 内部分配编码器、buffer 和 RTSP session。
int InitRtspEndpoint(RtspEndpoint endpoint, int port, const Options& options) {
  const int input_width = OutputWidth(options);
  const int input_height = OutputHeight(options);
  const int venc_rotate = 0;

  // 图像方向由相机输出配置决定，RTSP 侧只负责编码已准备好的 NV12 数据。
  switch (endpoint) {
    case RtspEndpoint::kCh1:
      return init_rtsp_ch1_rotate(input_width, input_height, options.fps, options.bps,
                                  port, const_cast<char*>(options.url.c_str()), venc_rotate);
    case RtspEndpoint::kCh2:
      return init_rtsp_ch2_rotate(input_width, input_height, options.fps, options.bps,
                                  port, const_cast<char*>(options.url.c_str()), venc_rotate);
    case RtspEndpoint::kCh3:
      return init_rtsp_ch3_rotate(input_width, input_height, options.fps, options.bps,
                                  port, const_cast<char*>(options.url.c_str()), venc_rotate);
    case RtspEndpoint::kCh4:
      return init_rtsp_ch4_rotate(input_width, input_height, options.fps, options.bps,
                                  port, const_cast<char*>(options.url.c_str()), venc_rotate);
    default:
      return -1;
  }
}

// 功能：关闭单个 RTSP endpoint。
// 输入：endpoint 指定需要关闭的 ch1..ch4。
// 输出：无。
// 副作用：释放该通道的 libprrtsp 上下文。
void CloseRtspEndpoint(RtspEndpoint endpoint) {
  switch (endpoint) {
    case RtspEndpoint::kCh1:
      rtspClose_ch1();
      break;
    case RtspEndpoint::kCh2:
      rtspClose_ch2();
      break;
    case RtspEndpoint::kCh3:
      rtspClose_ch3();
      break;
    case RtspEndpoint::kCh4:
      rtspClose_ch4();
      break;
    default:
      break;
  }
}

// 功能：关闭所有已初始化的 RTSP 通道。
// 输入：initialized_endpoints 保存成功初始化的通道；initialized_channels 为有效数量。
// 输出：无。
// 副作用：按反向初始化顺序释放 RTSP 资源。
void CloseRtspChannels(const std::array<RtspEndpoint, kMaxChannels>& initialized_endpoints,
                       int initialized_channels) {
  for (int channel = initialized_channels - 1; channel >= 0; --channel) {
    CloseRtspEndpoint(initialized_endpoints[channel]);
  }
}

}  // namespace robobaton_demo
