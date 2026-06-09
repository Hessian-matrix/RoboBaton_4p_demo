#pragma once

#include <array>

#include "cam_demo_common.h"

namespace robobaton_demo {

// 功能：初始化一路 RTSP 编码和推流上下文。
// 输入：endpoint 选择 ch1..ch4，port 为监听端口，options 提供分辨率、fps、码率和 URL。
// 输出：0 表示成功，非 0 表示 libprrtsp 初始化失败。
int InitRtspEndpoint(RtspEndpoint endpoint, int port, const Options& options);

// 功能：关闭已经成功初始化的 RTSP 通道。
// 输入：initialized_endpoints 记录初始化顺序，initialized_channels 为有效数量。
// 副作用：释放 libprrtsp 内部编码器和 RTSP session 资源。
void CloseRtspChannels(const std::array<RtspEndpoint, kMaxChannels>& initialized_endpoints,
                       int initialized_channels);

// 功能：把一帧 NV12 图像送入指定 RTSP endpoint。
// 输入：frame 内含 Y/UV 虚拟地址、物理地址和 RTSP PTS。
// 副作用：调用 libprrtsp 编码发送；不会接管 QueuedFrame 的释放。
void SendFrameToRtspEndpoint(RtspEndpoint endpoint, const QueuedFrame& frame);

}  // namespace robobaton_demo
