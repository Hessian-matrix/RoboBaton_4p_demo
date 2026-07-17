#include "release008_fake_producers.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <unistd.h>

struct icm42688_handle {
  icm42688_sample_callback_t callback = nullptr;
  void* user = nullptr;
  bool running = false;
};

struct sc132_frame {
  sc132_frame_info_t info{};
  std::vector<unsigned char> y;
  std::vector<unsigned char> uv;
  int references = 1;
};

struct prrtsp_stream {
  uint32_t camera_id = 0;
};

namespace {

std::mutex g_mutex;
std::vector<std::string> g_events;

int32_t g_sc_start_status = SC132_STATUS_OK;
sc132_frame_set_config_t g_sc_config{};
uint32_t g_sc_mask = 0;
int g_retain_count = 0;
int g_release_count = 0;
int g_sc_stop_count = 0;

std::array<int32_t, 4> g_open_status{};
std::array<bool, 4> g_open_return_handle{};
std::array<int32_t, 4> g_send_status{};
std::array<int32_t, 4> g_status_result{};
std::array<int, 4> g_close_failures{};
std::array<int, 4> g_send_count{};
std::array<int, 4> g_close_count{};
std::array<prrtsp_stream_config_v2, 4> g_rtsp_configs{};
std::array<prrtsp_nv12_frame_v2, 4> g_rtsp_frames{};
std::vector<prrtsp_stream*> g_streams;

int g_icm_create_status = ICM42688_STATUS_OK;
bool g_icm_create_return_handle = true;
int g_icm_set_callback_status = ICM42688_STATUS_OK;
int g_icm_start_status = ICM42688_STATUS_OK;
int g_icm_stop_status = ICM42688_STATUS_OK;
bool g_icm_emit_on_start = true;
uint32_t g_icm_start_burst_count = 1U;
bool g_icm_emit_during_stop = true;

void Record(const std::string& event) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_events.push_back(event);
  // join-failure gate 在 _Exit 子进程外观察调用轨迹；
  // trace 直接写 fd，不依赖析构或缓冲区 flush。
  const char* trace = std::getenv("RELEASE008_FAKE_TRACE");
  if (trace != nullptr && trace[0] != '\0') {
    const std::string line = "FAKE_EVENT " + event + "\n";
    const ssize_t written = ::write(STDERR_FILENO, line.data(), line.size());
    (void)written;
  }
}

uint32_t CameraFromPort(uint32_t port) {
  return port >= 554U && port <= 557U ? port - 554U : 0U;
}

icm42688_sample_t Sample(uint64_t timestamp_ns) {
  icm42688_sample_t sample{};
  sample.struct_size = sizeof(sample);
  sample.host_timestamp_ns = timestamp_ns;
  sample.temperature_c = 25.5;
  sample.accel_mps2[0] = 1.0;
  sample.accel_mps2[1] = 2.0;
  sample.accel_mps2[2] = 3.0;
  sample.gyro_rps[0] = 0.1;
  sample.gyro_rps[1] = 0.2;
  sample.gyro_rps[2] = 0.3;
  return sample;
}

}  // namespace

namespace release008_fake {

void Reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (prrtsp_stream* stream : g_streams) {
    delete stream;
  }
  g_streams.clear();
  g_events.clear();
  g_sc_start_status = SC132_STATUS_OK;
  g_sc_config = {};
  g_sc_mask = 0;
  g_retain_count = 0;
  g_release_count = 0;
  g_sc_stop_count = 0;
  g_open_status.fill(PRRTSP_OK);
  g_open_return_handle.fill(true);
  g_send_status.fill(PRRTSP_OK);
  g_status_result.fill(PRRTSP_OK);
  g_close_failures.fill(0);
  g_send_count.fill(0);
  g_close_count.fill(0);
  g_rtsp_configs = {};
  g_rtsp_frames = {};
  g_icm_create_status = ICM42688_STATUS_OK;
  g_icm_create_return_handle = true;
  g_icm_set_callback_status = ICM42688_STATUS_OK;
  g_icm_start_status = ICM42688_STATUS_OK;
  g_icm_stop_status = ICM42688_STATUS_OK;
  g_icm_emit_on_start = true;
  g_icm_start_burst_count = 1U;
  g_icm_emit_during_stop = true;
}

std::vector<std::string> Events() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_events;
}

int EventCount(const std::string& event) {
  const auto events = Events();
  return static_cast<int>(std::count(events.begin(), events.end(), event));
}

void SetScStartStatus(int32_t status) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sc_start_status = status;
}

sc132_frame_t* MakeFrame(uint32_t camera_id, uint32_t width, uint32_t height,
                         uint32_t stride, uint32_t vstride, uint64_t y_size,
                         uint64_t uv_size, uint64_t timestamp_ns) {
  auto* frame = new sc132_frame;
  frame->y.resize(static_cast<size_t>(y_size));
  frame->uv.resize(static_cast<size_t>(uv_size));
  frame->info.struct_size = sizeof(frame->info);
  frame->info.camera_id = camera_id;
  frame->info.sequence = 1000U + camera_id;
  frame->info.frame_id = 77U;
  frame->info.timestamp_ns = timestamp_ns + camera_id;
  frame->info.y_data = frame->y.empty() ? nullptr : frame->y.data();
  frame->info.uv_data = frame->uv.empty() ? nullptr : frame->uv.data();
  frame->info.y_phys = 0x100000U + camera_id * 0x1000U;
  frame->info.uv_phys = 0x200000U + camera_id * 0x1000U;
  frame->info.y_size = y_size;
  frame->info.uv_size = uv_size;
  frame->info.width = width;
  frame->info.height = height;
  frame->info.stride = stride;
  frame->info.vstride = vstride;
  return frame;
}

void DestroyFrame(sc132_frame_t* frame) { delete frame; }

void EmitFrameSet(const std::vector<sc132_frame_t*>& frames) {
  sc132_frame_set_callback_t callback = nullptr;
  void* user = nullptr;
  sc132_frame_set_t frame_set{};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    callback = g_sc_config.callback;
    user = g_sc_config.user_data;
  }
  frame_set.struct_size = sizeof(frame_set);
  frame_set.camera_count = static_cast<uint32_t>(frames.size());
  frame_set.group_id = 42U;
  frame_set.group_timestamp_ns = 123456789ULL;
  frame_set.max_skew_ns = 3U;
  for (size_t i = 0; i < frames.size() && i < SC132_FRAME_SET_MAX_CAMERAS; ++i) {
    sc132_frame* frame = frames[i];
    frame_set.items[i].frame = frame;
    frame_set.items[i].camera_id = frame->info.camera_id;
    frame_set.items[i].frame_id = frame->info.frame_id;
    frame_set.items[i].sequence = frame->info.sequence;
    frame_set.items[i].timestamp_ns = frame->info.timestamp_ns;
    frame_set.items[i].width = frame->info.width;
    frame_set.items[i].height = frame->info.height;
  }
  Record("sc_callback_enter");
  if (callback != nullptr) {
    callback(&frame_set, user);
  }
  Record("sc_callback_return");
}

int RetainCount() { std::lock_guard<std::mutex> lock(g_mutex); return g_retain_count; }
int ReleaseCount() { std::lock_guard<std::mutex> lock(g_mutex); return g_release_count; }
int ScStopCount() { std::lock_guard<std::mutex> lock(g_mutex); return g_sc_stop_count; }
sc132_frame_set_config_t LastScConfig() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_sc_config;
}

void SetRtspOpenStatus(uint32_t id, int32_t status, bool return_handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_open_status.at(id) = status;
  g_open_return_handle.at(id) = return_handle;
}
void SetRtspSendStatus(uint32_t id, int32_t status) {
  std::lock_guard<std::mutex> lock(g_mutex); g_send_status.at(id) = status;
}
void SetRtspStatusResult(uint32_t id, int32_t status) {
  std::lock_guard<std::mutex> lock(g_mutex); g_status_result.at(id) = status;
}
void SetRtspCloseFailures(uint32_t id, int failures) {
  std::lock_guard<std::mutex> lock(g_mutex); g_close_failures.at(id) = failures;
}
prrtsp_stream_config_v2 LastRtspConfig(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex); return g_rtsp_configs.at(id);
}
prrtsp_nv12_frame_v2 LastRtspFrame(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex); return g_rtsp_frames.at(id);
}
int RtspSendCount(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex); return g_send_count.at(id);
}
int RtspCloseCount(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex); return g_close_count.at(id);
}

void SetIcmCreateStatus(int status, bool return_handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_icm_create_status = status;
  g_icm_create_return_handle = return_handle;
}
void SetIcmSetCallbackStatus(int status) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_set_callback_status = status;
}
void SetIcmStartStatus(int status) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_start_status = status;
}
void SetIcmStopStatus(int status) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_stop_status = status;
}
void SetIcmEmitOnStart(bool enabled) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_emit_on_start = enabled;
}
void SetIcmStartBurstCount(uint32_t count) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_start_burst_count = count;
}
void SetIcmEmitDuringStop(bool enabled) {
  std::lock_guard<std::mutex> lock(g_mutex); g_icm_emit_during_stop = enabled;
}

}  // namespace release008_fake

extern "C" {

int32_t sc132_set_fps(uint32_t) {
  Record("sc_set_fps");
  return SC132_STATUS_OK;
}

int32_t sc132_set_output_rotation(uint32_t) {
  Record("sc_set_rotation");
  return SC132_STATUS_OK;
}

int32_t sc132_start_frame_set(const sc132_frame_set_config_t* config, uint32_t mask) {
  Record("sc_start");
  std::lock_guard<std::mutex> lock(g_mutex);
  if (config != nullptr) {
    g_sc_config = *config;
  }
  g_sc_mask = mask;
  return g_sc_start_status;
}

int32_t sc132_frame_retain(sc132_frame_t* frame) {
  Record("sc_retain");
  if (frame == nullptr) {
    return SC132_STATUS_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  ++frame->references;
  ++g_retain_count;
  return SC132_STATUS_OK;
}

void sc132_frame_release(sc132_frame_t* frame) {
  Record("sc_release");
  if (frame != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    --frame->references;
    ++g_release_count;
  }
}

int32_t sc132_frame_get_info(const sc132_frame_t* frame, sc132_frame_info_t* out_info) {
  Record("sc_get_info");
  if (frame == nullptr || out_info == nullptr || out_info->struct_size != sizeof(*out_info)) {
    return SC132_STATUS_INVALID_ARGUMENT;
  }
  *out_info = frame->info;
  return SC132_STATUS_OK;
}

void sc132_request_stop(void) { Record("sc_request_stop"); }

void sc132_stop(void) {
  Record("sc_stop");
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_sc_stop_count;
}

int32_t prrtsp_stream_open(const prrtsp_stream_config_v2* config, prrtsp_stream_t** out_stream) {
  if (config == nullptr || out_stream == nullptr) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }
  const uint32_t id = CameraFromPort(config->port);
  Record("rtsp_open_" + std::to_string(id));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_rtsp_configs.at(id) = *config;
  *out_stream = nullptr;
  if (g_open_return_handle.at(id)) {
    auto* stream = new prrtsp_stream;
    stream->camera_id = id;
    g_streams.push_back(stream);
    *out_stream = stream;
  }
  return g_open_status.at(id);
}

int32_t prrtsp_stream_send(prrtsp_stream_t* stream, const prrtsp_nv12_frame_v2* frame) {
  if (stream == nullptr || frame == nullptr) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }
  const uint32_t id = stream->camera_id;
  Record("rtsp_send_" + std::to_string(id));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_rtsp_frames.at(id) = *frame;
  ++g_send_count.at(id);
  return g_send_status.at(id);
}

int32_t prrtsp_stream_get_status(prrtsp_stream_t* stream, prrtsp_stream_status_v2* status) {
  if (stream == nullptr || status == nullptr) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }
  const uint32_t id = stream->camera_id;
  Record("rtsp_status_" + std::to_string(id));
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_status_result.at(id) == PRRTSP_OK) {
    const uint32_t size = status->struct_size;
    *status = {};
    status->struct_size = size;
    status->state = PRRTSP_STREAM_OPEN;
    status->frames_accepted = static_cast<uint64_t>(g_send_count.at(id));
  }
  return g_status_result.at(id);
}

int32_t prrtsp_stream_close(prrtsp_stream_t** stream) {
  if (stream == nullptr || *stream == nullptr) {
    return PRRTSP_E_INVALID_ARGUMENT;
  }
  const uint32_t id = (*stream)->camera_id;
  Record("rtsp_close_" + std::to_string(id));
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_close_count.at(id);
  if (g_close_failures.at(id) > 0) {
    --g_close_failures.at(id);
    return PRRTSP_E_BUSY;
  }
  auto it = std::find(g_streams.begin(), g_streams.end(), *stream);
  if (it != g_streams.end()) {
    g_streams.erase(it);
  }
  delete *stream;
  *stream = nullptr;
  return PRRTSP_OK;
}

int icm42688_create(const icm42688_config_t*, icm42688_handle_t** out_handle) {
  Record("icm_create");
  if (out_handle == nullptr) {
    return ICM42688_STATUS_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  *out_handle = g_icm_create_return_handle ? new icm42688_handle : nullptr;
  return g_icm_create_status;
}

int icm42688_set_callback(icm42688_handle_t* handle, icm42688_sample_callback_t callback,
                          void* user_data) {
  Record("icm_set_callback");
  int status = ICM42688_STATUS_OK;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    status = g_icm_set_callback_status;
  }
  if (status == ICM42688_STATUS_OK && handle != nullptr) {
    handle->callback = callback;
    handle->user = user_data;
  }
  return status;
}

int icm42688_start(icm42688_handle_t* handle) {
  Record("icm_start");
  if (handle == nullptr) {
    return ICM42688_STATUS_INVALID_ARGUMENT;
  }
  int status = ICM42688_STATUS_OK;
  bool emit = false;
  uint32_t burst_count = 0U;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    status = g_icm_start_status;
    emit = g_icm_emit_on_start;
    burst_count = g_icm_start_burst_count;
  }
  if (status != ICM42688_STATUS_OK) {
    return status;
  }
  handle->running = true;
  if (emit && handle->callback != nullptr) {
    // 同步 burst 的单调时间戳用于区分 FIFO 与单槽覆盖语义。
    for (uint32_t index = 0U; index < burst_count; ++index) {
      const icm42688_sample_t sample = Sample(100U + 10000000ULL * index);
      handle->callback(&sample, handle->user);
    }
  }
  return ICM42688_STATUS_OK;
}

int icm42688_stop(icm42688_handle_t* handle) {
  Record("icm_stop");
  if (handle == nullptr) {
    return ICM42688_STATUS_INVALID_ARGUMENT;
  }
  bool emit = false;
  int status = ICM42688_STATUS_OK;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    emit = g_icm_emit_during_stop;
    status = g_icm_stop_status;
  }
  if (emit && handle->callback != nullptr) {
    const icm42688_sample_t sample = Sample(200U);
    handle->callback(&sample, handle->user);
    Record("icm_stop_callback_return");
  }
  handle->running = false;
  return status;
}

int icm42688_is_running(const icm42688_handle_t* handle) {
  return handle != nullptr && handle->running ? 1 : 0;
}

void icm42688_destroy(icm42688_handle_t* handle) {
  Record("icm_destroy");
  delete handle;
}

const char* icm42688_status_message(int) { return "fake"; }

}  // extern "C"
