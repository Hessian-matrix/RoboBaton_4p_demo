#include "cam_demo_common.h"
#include "cam_demo_pipeline.h"
#include "cam_demo_rtsp.h"
#include "release008_fake_producers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

int release008_cam_demo_main(int argc, char** argv);

namespace {

using robobaton_demo::FinishSc132Shutdown;
using robobaton_demo::FramePipeline;
using robobaton_demo::ImuConsumerOptions;
using robobaton_demo::Options;
using robobaton_demo::PipelineHooks;
using robobaton_demo::RtspChannels;
using robobaton_demo::RunIcmConsumer;

int g_checks = 0;

void Check(bool condition, const char* message) {
  ++g_checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Predicate>
void WaitUntil(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Options OneCameraOptions() {
  Options options;
  options.channels = 1;
  options.camera_mask = 1U;
  options.width = 1280;
  options.height = 1088;
  options.fps = 60;
  options.bps = 4000;
  options.url = "/PRR";
  options.rotate_degrees = 0;
  options.frame_set_timeout_ms = 100U;
  options.frame_set_max_skew_ns = 1000000ULL;
  return options;
}

sc132_frame_t* PackedFrame(uint64_t y_size = 1280ULL * 1088ULL,
                           uint64_t uv_size = 1280ULL * 544ULL) {
  return release008_fake::MakeFrame(0U, 1280U, 1088U, 1280U, 1088U, y_size, uv_size);
}

void AssertZero(const uint64_t* values, size_t count, const char* message) {
  for (size_t i = 0; i < count; ++i) {
    Check(values[i] == 0U, message);
  }
}

void TestRtspFullConfigAndDescriptor() {
  release008_fake::Reset();
  const Options options = OneCameraOptions();
  RtspChannels rtsp;
  Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "RTSP open failed");
  Check(rtsp.OpenHandleCount() == 1U, "RTSP handle was not saved");

  const prrtsp_stream_config_v2 config = release008_fake::LastRtspConfig(0);
  Check(config.struct_size == PRRTSP_STREAM_CONFIG_V2_0_SIZE, "config struct_size");
  Check(config.flags == 0U, "config flags");
  Check(config.width == 1280U && config.height == 1088U, "config dimensions");
  Check(config.fps_num == 60U && config.fps_den == 1U, "config fps");
  Check(config.bitrate_kbps == 4000U, "config bitrate");
  Check(config.rotation_clockwise == 0U, "config rotation");
  Check(config.port == 554U, "config port");
  Check(config.operation_timeout_ms == 1000U, "config timeout");
  Check(std::string(config.path) == "/PRR", "config path");
  AssertZero(config.reserved, 8U, "config reserved");

  FramePipeline pipeline(options, &rtsp);
  pipeline.StartWorkers();
  const sc132_frame_set_config_t sc_config = pipeline.MakeFrameSetConfig();
  Check(sc_config.struct_size == sizeof(sc_config), "SC config struct_size");
  Check(sc_config.callback != nullptr && sc_config.user_data == &pipeline, "SC callback context");
  Check(sc_config.camera_count == 1U, "SC camera_count");
  Check(sc_config.width == 1280U && sc_config.height == 1088U, "SC output dimensions");
  Check(sc_config.timeout_ms == 100U, "SC timeout");
  Check(sc_config.max_skew_ns == 1000000ULL, "SC max skew");
  AssertZero(reinterpret_cast<const uint64_t*>(sc_config.reserved), 4U, "SC reserved");
  Check(sc132_start_frame_set(&sc_config, options.camera_mask) == SC132_STATUS_OK, "SC start");

  sc132_frame_t* frame = PackedFrame();
  release008_fake::EmitFrameSet({frame});
  WaitUntil([] { return release008_fake::RtspSendCount(0) == 1; }, "RTSP send timeout");
  Check(FinishSc132Shutdown(&pipeline), "normal SC shutdown failed");
  Check(release008_fake::ScStopCount() == 2, "SC stop must be exactly twice");
  Check(rtsp.CaptureStatuses(), "RTSP status failed");
  Check(rtsp.CloseReverse(), "RTSP close failed");

  const prrtsp_nv12_frame_v2 descriptor = release008_fake::LastRtspFrame(0);
  Check(descriptor.struct_size == PRRTSP_NV12_FRAME_V2_0_SIZE, "frame struct_size");
  Check(descriptor.flags == 0U, "frame flags");
  Check(descriptor.width == 1280U && descriptor.height == 1088U, "frame dimensions");
  Check(descriptor.y_stride == 1280U && descriptor.uv_stride == 1280U, "frame stride");
  Check(descriptor.y_vstride == 1088U && descriptor.uv_vstride == 544U, "frame vstride");
  Check(descriptor.y_virtual_address != 0U && descriptor.uv_virtual_address != 0U,
        "frame virtual addresses");
  Check(descriptor.y_physical_address == 0U && descriptor.uv_physical_address == 0U,
        "frame physical addresses must be zero");
  Check(descriptor.y_size_bytes == 1280ULL * 1088ULL, "frame y size");
  Check(descriptor.uv_size_bytes == 1280ULL * 544ULL, "frame uv size");
  Check(descriptor.timestamp_ns == 123456789ULL, "frame timestamp");
  AssertZero(descriptor.reserved, 8U, "frame reserved");
  Check(release008_fake::RetainCount() == 1, "retain count");
  Check(release008_fake::ReleaseCount() == 1, "release count");
  release008_fake::DestroyFrame(frame);
}

void ThrowFrameSet(const sc132_frame_set_t&, void*) { throw std::runtime_error("callback"); }
void ThrowQueuedFrame(const robobaton_demo::QueuedFrame&, void*) {
  throw std::runtime_error("worker");
}
void ThrowAllocation(void*) { throw std::bad_alloc(); }

bool JoinThenReportFailure(std::thread& worker, void*) {
  if (worker.joinable()) {
    worker.join();
  }
  return false;
}

struct WorkerCreateFailureState {
  int calls = 0;
};

std::thread CreateOneWorkerThenFail(std::function<void()> entry, void* user) {
  auto* state = static_cast<WorkerCreateFailureState*>(user);
  ++state->calls;
  if (state->calls == 2) {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                            "injected std::thread create failure");
  }
  return std::thread(std::move(entry));
}

void TestWorkerCreateFailureHasNoIdleScRequestAndReapsPartialThread() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  options.channels = 4;
  options.camera_mask = 0x0fU;
  RtspChannels rtsp;
  WorkerCreateFailureState state;
  PipelineHooks hooks{};
  hooks.create_thread = CreateOneWorkerThenFail;
  hooks.user = &state;
  FramePipeline pipeline(options, &rtsp, hooks);

  bool threw = false;
  try {
    pipeline.StartWorkers();
  } catch (const std::system_error&) {
    threw = true;
  }
  Check(threw && state.calls == 2, "worker create fault was not injected after partial create");
  Check(release008_fake::EventCount("sc_request_stop") == 0,
        "worker create failure issued idle SC request_stop");
  Check(release008_fake::ScStopCount() == 0, "worker create failure issued idle SC stop");
  pipeline.BeginShutdown(false);
  Check(pipeline.Join(), "partial worker create cleanup did not join created worker");
  Check(pipeline.IsQuiescent(), "partial worker create cleanup did not become quiescent");
  Check(pipeline.OwnedThreadCountForTesting() == 0U,
        "partial worker create cleanup retained a thread resource");
}

struct BlockingWorkerState {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
};

void BlockQueuedFrame(const robobaton_demo::QueuedFrame&, void* user) {
  auto* state = static_cast<BlockingWorkerState*>(user);
  state->entered.store(true, std::memory_order_release);
  while (!state->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void TestQueueStopAndDrainReleasesCurrentAndQueued() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  RtspChannels rtsp;
  Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "open for drain");
  BlockingWorkerState state;
  PipelineHooks hooks{};
  hooks.on_queued_frame = BlockQueuedFrame;
  hooks.user = &state;
  FramePipeline pipeline(options, &rtsp, hooks);
  pipeline.StartWorkers();
  const auto config = pipeline.MakeFrameSetConfig();
  Check(sc132_start_frame_set(&config, 1U) == SC132_STATUS_OK, "start drain");
  sc132_frame_t* frame = PackedFrame();
  release008_fake::EmitFrameSet({frame});
  WaitUntil([&] { return state.entered.load(std::memory_order_acquire); },
            "worker did not own current frame");
  release008_fake::EmitFrameSet({frame});
  release008_fake::EmitFrameSet({frame});
  pipeline.BeginShutdown(true);
  WaitUntil([] { return release008_fake::ReleaseCount() >= 2; }, "queued frames not drained");
  state.release.store(true, std::memory_order_release);
  Check(FinishSc132Shutdown(&pipeline), "drain shutdown");
  Check(release008_fake::RetainCount() == 3 && release008_fake::ReleaseCount() == 3,
        "current/queued frames were not released exactly once");
  Check(rtsp.CaptureStatuses(), "drain status");
  Check(rtsp.CloseReverse(), "drain close");
  release008_fake::DestroyFrame(frame);
}

void TestInvalidFrameSetAndStartupFailure() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  RtspChannels rtsp;
  Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "open invalid frame-set");
  FramePipeline pipeline(options, &rtsp);
  pipeline.StartWorkers();
  const auto config = pipeline.MakeFrameSetConfig();
  Check(sc132_start_frame_set(&config, 1U) == SC132_STATUS_OK, "start invalid frame-set");
  sc132_frame_set_t invalid{};
  invalid.struct_size = 0U;
  invalid.camera_count = 1U;
  config.callback(&invalid, config.user_data);
  WaitUntil([&] { return pipeline.FirstError() != 0; }, "invalid frame-set not rejected");
  Check(FinishSc132Shutdown(&pipeline), "invalid frame-set shutdown");
  Check(release008_fake::ScStopCount() == 2, "invalid frame-set stop count");
  Check(rtsp.CaptureStatuses() && rtsp.CloseReverse(), "invalid frame-set RTSP cleanup");

  release008_fake::Reset();
  RtspChannels failed_rtsp;
  Check(failed_rtsp.Open(0, 554, options) == PRRTSP_OK, "open startup failure");
  FramePipeline failed_pipeline(options, &failed_rtsp);
  failed_pipeline.StartWorkers();
  const auto failed_config = failed_pipeline.MakeFrameSetConfig();
  release008_fake::SetScStartStatus(SC132_STATUS_STARTUP_FAILED);
  Check(sc132_start_frame_set(&failed_config, 1U) == SC132_STATUS_STARTUP_FAILED,
        "fake startup failure missing");
  Check(FinishSc132Shutdown(&failed_pipeline), "startup failure shutdown");
  Check(release008_fake::ScStopCount() == 2, "startup failure must stop twice");
  Check(failed_rtsp.CaptureStatuses() && failed_rtsp.CloseReverse(),
        "startup failure RTSP cleanup");
}

void TestCallbackAndQueueFailureFirewalls() {
  for (int mode = 0; mode < 2; ++mode) {
    release008_fake::Reset();
    Options options = OneCameraOptions();
    RtspChannels rtsp;
    Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "open for callback failure");
    PipelineHooks hooks{};
    if (mode == 0) {
      hooks.on_frame_set = ThrowFrameSet;
    } else {
      hooks.before_queue_insert = ThrowAllocation;
    }
    FramePipeline pipeline(options, &rtsp, hooks);
    pipeline.StartWorkers();
    const auto config = pipeline.MakeFrameSetConfig();
    Check(sc132_start_frame_set(&config, 1U) == SC132_STATUS_OK, "start callback failure");
    sc132_frame_t* frame = PackedFrame();
    release008_fake::EmitFrameSet({frame});
    WaitUntil([&] { return pipeline.FirstError() != 0; }, "callback failure not recorded");
    Check(release008_fake::EventCount("sc_callback_return") == 1,
          "exception crossed SC callback boundary");
    Check(release008_fake::EventCount("sc_request_stop") >= 1,
          "callback failure did not request stop");
    Check(release008_fake::ScStopCount() == 0, "callback called blocking stop");
    Check(FinishSc132Shutdown(&pipeline), "callback failure shutdown");
    Check(release008_fake::ScStopCount() == 2, "callback failure stop count");
    if (mode == 1) {
      Check(release008_fake::RetainCount() == 1 && release008_fake::ReleaseCount() == 1,
            "allocation failure lost retained frame");
    }
    Check(rtsp.CaptureStatuses(), "callback failure status");
    Check(rtsp.CloseReverse(), "callback failure close");
    release008_fake::DestroyFrame(frame);
  }
}

void TestWorkerAndSendFailures() {
  for (int mode = 0; mode < 3; ++mode) {
    release008_fake::Reset();
    Options options = OneCameraOptions();
    RtspChannels rtsp;
    Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "open for worker failure");
    PipelineHooks hooks{};
    if (mode == 0) {
      hooks.on_queued_frame = ThrowQueuedFrame;
    } else if (mode == 1) {
      release008_fake::SetRtspSendStatus(0, PRRTSP_E_CODEC);
    }
    FramePipeline pipeline(options, &rtsp, hooks);
    pipeline.StartWorkers();
    const auto config = pipeline.MakeFrameSetConfig();
    Check(sc132_start_frame_set(&config, 1U) == SC132_STATUS_OK, "start worker failure");
    sc132_frame_t* frame = mode == 2 ? PackedFrame(1280ULL * 1088ULL - 1U) : PackedFrame();
    release008_fake::EmitFrameSet({frame});
    WaitUntil([&] { return pipeline.FirstError() != 0; }, "worker/send failure not recorded");
    Check(FinishSc132Shutdown(&pipeline), "worker/send failure shutdown");
    Check(release008_fake::ReleaseCount() == 1, "worker/send current frame release");
    Check(pipeline.TotalSentFrames(0) == 0U, "failed send counted as success");
    if (mode == 1) {
      Check(pipeline.FirstError() == PRRTSP_E_CODEC, "send status not preserved");
    }
    if (mode == 2) {
      Check(release008_fake::RtspSendCount(0) == 0, "undersized descriptor reached producer");
    }
    Check(rtsp.CaptureStatuses(), "worker failure status");
    Check(rtsp.CloseReverse(), "worker failure close");
    release008_fake::DestroyFrame(frame);
  }
}

void TestJoinFailureIsFailClosed() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  RtspChannels rtsp;
  Check(rtsp.Open(0, 554, options) == PRRTSP_OK, "open for join failure");
  PipelineHooks hooks{};
  hooks.join_thread = JoinThenReportFailure;
  FramePipeline pipeline(options, &rtsp, hooks);
  pipeline.StartWorkers();
  const auto config = pipeline.MakeFrameSetConfig();
  Check(sc132_start_frame_set(&config, 1U) == SC132_STATUS_OK, "start join failure");
  Check(!FinishSc132Shutdown(&pipeline), "join failure reported success");
  Check(!pipeline.IsQuiescent(), "join failure lost ownership state");
  Check(release008_fake::ScStopCount() == 0, "join failure called producer stop");
  Check(release008_fake::RtspCloseCount(0) == 0, "join failure closed RTSP");
}

std::string RunRealMainJoinFailureChild(int* exit_code) {
  int output_pipe[2] = {-1, -1};
  Check(pipe(output_pipe) == 0, "join-gate pipe failed");
  const pid_t child = fork();
  Check(child >= 0, "join-gate fork failed");
  if (child == 0) {
    (void)close(output_pipe[0]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(output_pipe[1], STDERR_FILENO) < 0) {
      _exit(120);
    }
    (void)close(output_pipe[1]);
    (void)setenv("RELEASE008_TEST_JOIN_FAILURE", "1", 1);
    (void)setenv("RELEASE008_FAKE_TRACE", "1", 1);
    char program[] = "cam_demo";
    char channels[] = "--channels";
    char one[] = "1";
    char* arguments[] = {program, channels, one, nullptr};
    const int returned = release008_cam_demo_main(3, arguments);
    static constexpr char kReturned[] = "REAL_MAIN_RETURNED_AFTER_JOIN_FAILURE\n";
    const ssize_t marker_written = ::write(STDERR_FILENO, kReturned, sizeof(kReturned) - 1U);
    (void)marker_written;
    _exit(returned);
  }

  (void)close(output_pipe[1]);
  std::string output;
  std::array<char, 1024> buffer{};
  while (true) {
    const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count == 0) {
      break;
    }
    if (count < 0) {
      (void)close(output_pipe[0]);
      throw std::runtime_error("join-gate pipe read failed");
    }
    output.append(buffer.data(), static_cast<size_t>(count));
  }
  (void)close(output_pipe[0]);
  int status = 0;
  Check(waitpid(child, &status, 0) == child, "join-gate waitpid failed");
  Check(WIFEXITED(status), "real cam_demo main did not exit normally");
  *exit_code = WEXITSTATUS(status);
  return output;
}

void TestRealMainJoinFailureSkipsRtspCleanupAndDoesNotReturn() {
  release008_fake::Reset();
  release008_fake::SetScStartStatus(SC132_STATUS_STARTUP_FAILED);
  int exit_code = -1;
  const std::string output = RunRealMainJoinFailureChild(&exit_code);
  Check(exit_code == 1, "real cam_demo main join failure did not exit 1");
  Check(output.find("fatal: consumer join failed; skipping RTSP status/close") !=
            std::string::npos,
        "real cam_demo main did not report skipped RTSP cleanup");
  Check(output.find("REAL_MAIN_RETURNED_AFTER_JOIN_FAILURE") == std::string::npos,
        "production cam_demo main returned after join failure");
  Check(output.find("FAKE_EVENT rtsp_open_0") != std::string::npos,
        "real cam_demo main did not reach RTSP ownership");
  Check(output.find("FAKE_EVENT rtsp_status_0") == std::string::npos,
        "join-failure main called RTSP status");
  Check(output.find("FAKE_EVENT rtsp_close_0") == std::string::npos,
        "join-failure main called RTSP close");
  Check(output.find("FAKE_EVENT sc_stop") == std::string::npos,
        "join-failure main called blocking SC stop before consumer quiescence");
}

void TestRtspCleanupRequiredAndCloseRetries() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  release008_fake::SetRtspOpenStatus(0, PRRTSP_E_CLEANUP_REQUIRED, true);
  RtspChannels cleanup_required;
  Check(cleanup_required.Open(0, 554, options) == PRRTSP_E_CLEANUP_REQUIRED,
        "cleanup-required status lost");
  Check(cleanup_required.OpenHandleCount() == 1U, "cleanup-required handle not saved");
  release008_fake::SetRtspCloseFailures(0, 2);
  Check(cleanup_required.CloseReverse(), "close retry did not succeed");
  Check(release008_fake::RtspCloseCount(0) == 3, "close retry total must be three");

  release008_fake::Reset();
  RtspChannels status_failure;
  Check(status_failure.Open(0, 554, options) == PRRTSP_OK, "open for status failure");
  release008_fake::SetRtspStatusResult(0, PRRTSP_E_RTSP);
  Check(!status_failure.CaptureStatuses(), "RTSP status failure reported success");
  Check(status_failure.CloseReverse(), "status failure close");

  release008_fake::Reset();
  RtspChannels retained;
  Check(retained.Open(0, 554, options) == PRRTSP_OK, "open for retained close");
  release008_fake::SetRtspCloseFailures(0, 3);
  Check(!retained.CloseReverse(), "third close failure reported success");
  Check(retained.OpenHandleCount() == 1U, "failed close discarded handle");
  Check(release008_fake::RtspCloseCount(0) == 3, "more/less than three close calls");
  release008_fake::Reset();
}

void TestRtspPathValidationAndFourPhysicalSlots() {
  release008_fake::Reset();
  Options options = OneCameraOptions();
  RtspChannels rtsp;
  for (int camera = 0; camera < 4; ++camera) {
    Check(rtsp.Open(camera, 554 + camera, options) == PRRTSP_OK, "four-handle open");
  }
  Check(rtsp.OpenHandleCount() == 4U, "not exactly four physical slots");
  Check(rtsp.CaptureStatuses(), "four status");
  Check(rtsp.CloseReverse(), "four close");
  const auto events = release008_fake::Events();
  auto position = [&](const std::string& event) {
    return std::find(events.begin(), events.end(), event) - events.begin();
  };
  Check(position("rtsp_close_3") < position("rtsp_close_2") &&
            position("rtsp_close_2") < position("rtsp_close_1") &&
            position("rtsp_close_1") < position("rtsp_close_0"),
        "RTSP close order not reverse physical id");

  for (const std::string& bad : {std::string("/"), std::string("/bad?query"),
                                 std::string("/bad#fragment"), std::string("/bad%20path"),
                                 std::string("/bad\\path"), std::string("/bad path"),
                                 std::string(57U, 'a')}) {
    release008_fake::Reset();
    Options invalid = options;
    invalid.url = bad;
    RtspChannels invalid_rtsp;
    Check(invalid_rtsp.Open(0, 554, invalid) == PRRTSP_E_INVALID_ARGUMENT,
          "forbidden RTSP path accepted");
    Check(release008_fake::EventCount("rtsp_open_0") == 0, "invalid path reached open");
  }
}

struct IcmObserverState {
  int calls = 0;
  bool throw_now = false;
  std::vector<uint64_t> timestamps;
};

void ObserveIcm(const icm42688_sample_t& sample, void* user) {
  auto* state = static_cast<IcmObserverState*>(user);
  ++state->calls;
  state->timestamps.push_back(sample.host_timestamp_ns);
  if (state->throw_now) {
    throw std::runtime_error("IMU observer");
  }
}

void TestIcmBurstOrderAndQueueFullFailClosed() {
  release008_fake::Reset();
  release008_fake::SetIcmStartBurstCount(8U);
  ImuConsumerOptions options;
  options.sample_rate_hz = 100U;
  options.count = 1U;
  IcmObserverState state;

  // 2026-07-16 修改原因：真实 RunIcmConsumer 必须按 FIFO 顺序交付 producer
  // 在 start 内同步产生的 burst；单槽实现会错误交付最后一个时间戳。
  Check(RunIcmConsumer(options, ObserveIcm, &state) == 0, "ICM burst run");
  Check(state.timestamps == std::vector<uint64_t>{100U},
        "ICM burst did not preserve oldest sample");

  release008_fake::Reset();
  release008_fake::SetIcmStartBurstCount(65U);
  state = {};
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0,
        "ICM full queue silently dropped or overwrote a sample");
  Check(state.calls == 0, "ICM full queue emitted after fail-closed admission");
}

void TestIcmLifecycleAndExceptionFirewall() {
  release008_fake::Reset();
  ImuConsumerOptions options;
  options.sample_rate_hz = 1000U;
  options.count = 1U;
  IcmObserverState state;
  Check(RunIcmConsumer(options, ObserveIcm, &state) == 0, "ICM normal run");
  Check(state.calls == 1, "ICM admission remained open during stop");
  const auto events = release008_fake::Events();
  const auto stop = std::find(events.begin(), events.end(), "icm_stop");
  const auto callback_return = std::find(events.begin(), events.end(), "icm_stop_callback_return");
  const auto destroy = std::find(events.begin(), events.end(), "icm_destroy");
  Check(stop < callback_return && callback_return < destroy, "ICM context/finalizer order");

  release008_fake::Reset();
  state = {};
  release008_fake::SetIcmStopStatus(ICM42688_STATUS_IO_ERROR);
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0, "ICM stop failure ignored");
  Check(release008_fake::EventCount("icm_destroy") == 1, "ICM stop failure skipped destroy");

  release008_fake::Reset();
  state = {};
  state.throw_now = true;
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0, "ICM callback exception ignored");
  Check(release008_fake::EventCount("icm_destroy") == 1, "ICM callback exception skipped destroy");

  release008_fake::Reset();
  release008_fake::SetIcmCreateStatus(ICM42688_STATUS_IO_ERROR, true);
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0, "ICM create failure ignored");
  Check(release008_fake::EventCount("icm_destroy") == 1, "non-null failed create not destroyed");

  release008_fake::Reset();
  release008_fake::SetIcmStartStatus(ICM42688_STATUS_IO_ERROR);
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0, "ICM start failure ignored");
  Check(release008_fake::EventCount("icm_destroy") == 1, "failed start not destroyed");

  release008_fake::Reset();
  release008_fake::SetIcmSetCallbackStatus(ICM42688_STATUS_IO_ERROR);
  Check(RunIcmConsumer(options, ObserveIcm, &state) != 0, "ICM set-callback failure ignored");
  Check(release008_fake::EventCount("icm_destroy") == 1,
        "set-callback failure skipped destroy");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    int repeat = 1;
    if (argc == 2 && std::string(argv[1]) == "--main-join-gate-only") {
      TestRealMainJoinFailureSkipsRtspCleanupAndDoesNotReturn();
      std::cout << "release008 real-main join gate PASS checks=" << g_checks << "\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--repeat") {
      repeat = std::stoi(argv[2]);
    }
    TestRtspPathValidationAndFourPhysicalSlots();
    TestRtspCleanupRequiredAndCloseRetries();
    TestCallbackAndQueueFailureFirewalls();
    TestWorkerAndSendFailures();
    TestQueueStopAndDrainReleasesCurrentAndQueued();
    TestInvalidFrameSetAndStartupFailure();
    TestWorkerCreateFailureHasNoIdleScRequestAndReapsPartialThread();
    TestJoinFailureIsFailClosed();
    TestRealMainJoinFailureSkipsRtspCleanupAndDoesNotReturn();
    TestIcmBurstOrderAndQueueFullFailClosed();
    TestIcmLifecycleAndExceptionFirewall();
    for (int iteration = 0; iteration < repeat; ++iteration) {
      TestRtspFullConfigAndDescriptor();
    }
    std::cout << "release008 lifecycle PASS checks=" << g_checks
              << " repeat=" << repeat << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "release008 lifecycle FAIL: " << error.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "release008 lifecycle FAIL: unknown exception\n";
    return 1;
  }
}
