#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "icm42688_x5/icm42688_driver.h"
#include "prrtsp_v2.h"
#include "sc132camera.h"
}

namespace release008_fake {

void Reset();
std::vector<std::string> Events();
int EventCount(const std::string& event);

void SetScStartStatus(int32_t status);
sc132_frame_t* MakeFrame(uint32_t camera_id, uint32_t width, uint32_t height,
                         uint32_t stride, uint32_t vstride, uint64_t y_size,
                         uint64_t uv_size, uint64_t timestamp_ns = 123456789ULL);
void DestroyFrame(sc132_frame_t* frame);
void EmitFrameSet(const std::vector<sc132_frame_t*>& frames);
int RetainCount();
int ReleaseCount();
int ScStopCount();
sc132_frame_set_config_t LastScConfig();

void SetRtspOpenStatus(uint32_t camera_id, int32_t status, bool return_handle);
void SetRtspSendStatus(uint32_t camera_id, int32_t status);
void SetRtspStatusResult(uint32_t camera_id, int32_t status);
void SetRtspCloseFailures(uint32_t camera_id, int failures);
prrtsp_stream_config_v2 LastRtspConfig(uint32_t camera_id);
prrtsp_nv12_frame_v2 LastRtspFrame(uint32_t camera_id);
int RtspSendCount(uint32_t camera_id);
int RtspCloseCount(uint32_t camera_id);

void SetIcmCreateStatus(int status, bool return_handle);
void SetIcmSetCallbackStatus(int status);
void SetIcmStartStatus(int status);
void SetIcmStopStatus(int status);
void SetIcmEmitOnStart(bool enabled);
void SetIcmStartBurstCount(uint32_t count);
void SetIcmEmitDuringStop(bool enabled);
void SetIcmAsyncProduction(uint32_t sample_count, uint32_t interval_us);
uint32_t IcmAsyncProducedCount();

}  // namespace release008_fake
