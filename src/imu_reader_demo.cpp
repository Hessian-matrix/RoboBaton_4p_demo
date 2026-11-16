#include "cam_demo_common.h"

#include <limits.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include "icm42670_driver.h"
}

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

namespace robobaton_demo {
volatile sig_atomic_t g_imu_signal_stop = 0;

namespace {
#ifdef RELEASE008_TESTING
// 测试仅统计空队列等待次数；原子计数避免并发读写竞争。
std::atomic<uint32_t> g_idle_wait_count{0U};
#endif


struct IcmCallbackContext {
  static constexpr uint64_t kPendingCapacity = 4096U;
  std::array<icm42670_sample_t, kPendingCapacity> pending_samples{};
  std::atomic<uint64_t> written{0U}, read{0U};
  std::atomic<bool> callback_failed{false};
  std::atomic<const char*> failure_reason{nullptr};
  uint64_t emitted = 0U;

  void Fail(const char* reason) noexcept {
    const char* expected = nullptr;
    failure_reason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
    callback_failed.store(true, std::memory_order_release);
  }

  bool Pop(icm42670_sample_t& sample) noexcept {
    const uint64_t index = read.load(std::memory_order_relaxed);
    if (index == written.load(std::memory_order_acquire)) return false;
    sample = pending_samples[index % kPendingCapacity];
    read.store(index + 1U, std::memory_order_release);
    return true;
  }
};
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "IMU adapter requires lock-free queue indices");

// Serial producer publishes fixed slots; only the consumer invokes observers.
void IcmCallback(const icm42670_sample_t* sample, void* user) noexcept {
  auto* context = static_cast<IcmCallbackContext*>(user);
  if (context == nullptr || context->callback_failed.load(std::memory_order_acquire)) return;
  if (sample == nullptr || sample->struct_size != sizeof(*sample)) {
    context->Fail("invalid_sample");
    return;
  }
  const uint64_t index = context->written.load(std::memory_order_relaxed);
  if (index - context->read.load(std::memory_order_acquire) ==
      IcmCallbackContext::kPendingCapacity) {
    context->Fail("sample_queue_full");
    return;
  }
  context->pending_samples[index % IcmCallbackContext::kPendingCapacity] = *sample;
  context->written.store(index + 1U, std::memory_order_release);
}

}  // namespace

#ifdef RELEASE008_TESTING
void ResetImuIdleWaitCountForTest() {
  g_idle_wait_count.store(0U, std::memory_order_release);
}

uint32_t ImuIdleWaitCountForTest() {
  return g_idle_wait_count.load(std::memory_order_acquire);
}

std::size_t ImuPendingCapacityForTest() {
  return IcmCallbackContext::kPendingCapacity;
}
#endif

int RunIcmConsumer(const ImuConsumerOptions& options, ImuSampleObserver observer,
                   void* observer_user) {
  if (options.final_health != nullptr) {
    *options.final_health = ICM42670_RUNTIME_HEALTH_INIT;
  }
  if (options.sample_rate_hz == 0U) return 1;

#ifndef RELEASE008_TESTING
  // The consumer drains below the library acquisition priority, including STOP.
  const sched_param scheduling{3};
  if (sched_setscheduler(0, SCHED_RR, &scheduling) != 0) {
    std::cerr << "fatal: schedule IMU consumer: " << std::strerror(errno) << "\n";
    return 1;
  }
#endif
  icm42670_config_t config = ICM42670_CONFIG_INIT;
  config.sample_rate_hz = options.sample_rate_hz;
  config.fifo_watermark_samples = 1U;
  config.sample_drop_policy = options.sample_drop_policy;
  config.timestamp_mode = options.timestamp_mode;
  config.fsync_diagnostics = options.fsync_diagnostics ? 1U : 0U;

  IcmCallbackContext context;
  icm42670_handle_t* handle = nullptr;
  const int create_result = icm42670_create(&config, &handle);
  if (create_result != ICM42670_STATUS_OK || handle == nullptr) {
    if (handle != nullptr) icm42670_destroy(handle);
    return 1;
  }
  if (icm42670_set_callback(handle, IcmCallback, &context) != ICM42670_STATUS_OK) {
    icm42670_destroy(handle);
    return 1;
  }

  const int start_result = icm42670_start(handle);
  bool owner_failed = start_result != ICM42670_STATUS_OK;
  const auto owner_stop_requested = [&]() {
    return g_imu_signal_stop != 0 ||
        (options.stop_requested != nullptr &&
         options.stop_requested->load(std::memory_order_acquire));
  };
  const auto emit_sample = [&](const icm42670_sample_t& sample) noexcept {
    try {
      if (observer != nullptr) observer(sample, observer_user);
      ++context.emitted;
    } catch (...) {
      context.Fail("sample_observer_exception");
    }
  };

  while (!owner_failed && !owner_stop_requested() &&
         !context.callback_failed.load(std::memory_order_acquire)) {
    icm42670_sample_t sample{};
    if (context.Pop(sample)) {
      emit_sample(sample);
      // Count requests STOP, not an admission cutoff: the final cohort may exceed it.
      if (options.count != 0U && context.emitted >= options.count) break;
      continue;
    }
    if (icm42670_is_running(handle) == 0) {
      owner_failed = true;
      break;
    }
#ifdef RELEASE008_TESTING
    g_idle_wait_count.fetch_add(1U, std::memory_order_relaxed);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  int stop_result = ICM42670_STATUS_INTERNAL_ERROR;
  std::atomic<bool> stopped{false};
  try {
    // STOP can publish more than one queue's worth: drain while it joins the producer.
    std::thread stop_owner([&]() {
      stop_result = icm42670_stop(handle);
      stopped.store(true, std::memory_order_release);
    });
    for (;;) {
      icm42670_sample_t sample{};
      if (context.Pop(sample)) {
        emit_sample(sample);
        continue;
      }
      if (stopped.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop_owner.join();
  } catch (...) {
    owner_failed = true;
    stop_result = icm42670_stop(handle);
  }
  // Drain even after fatal STOP. The admitted prefix remains owned by the adapter.
  icm42670_sample_t sample{};
  while (context.Pop(sample)) emit_sample(sample);

  icm42670_runtime_health_t health = ICM42670_RUNTIME_HEALTH_INIT;
  const int health_result = icm42670_get_runtime_health(handle, &health);
  if (options.final_health != nullptr) *options.final_health = health;
  const bool terminal_failed = health_result != ICM42670_STATUS_OK ||
      health.session_generation == 0U || health.transport_complete == 0U ||
      health.fatal_reason != 0U || health.fatal_errno != 0U ||
      health.quiesce_errno != 0U || health.fifo_overflow_count != 0U ||
      health.scan_push_failure_count != 0U || health.evidence_full_count != 0U ||
      health.records_consumed != health.records_acked ||
      health.published_samples != context.emitted ||
      (options.sample_drop_policy == ICM42670_SAMPLE_DROP_POLICY_STRICT &&
       health.invalid_payload_count != 0U);
  const bool failed = owner_failed || terminal_failed ||
      context.callback_failed.load(std::memory_order_acquire) ||
      stop_result != ICM42670_STATUS_OK;
  if (failed) {
    const char* reason = context.failure_reason.load(std::memory_order_acquire);
    std::cerr << "IMU_CONSUMER_FAILURE reason=" << (reason ? reason : "producer_lifecycle")
              << " emitted=" << context.emitted
              << " admitted=" << context.written.load(std::memory_order_acquire)
              << " stop_status=" << stop_result << " health_status=" << health_result << '\n';
  }
  icm42670_destroy(handle);
  return failed ? 1 : 0;
}

uint32_t ImuPrintEverySamples(uint32_t sample_rate_hz, uint32_t print_rate_hz) {
  if (sample_rate_hz == 0U || print_rate_hz == 0U) {
    return 0U;
  }
  // 用商和余数实现向上取整，避免两个 uint32_t 相加溢出。
  return sample_rate_hz / print_rate_hz +
         (sample_rate_hz % print_rate_hz == 0U ? 0U : 1U);
}

void PrintImuSample(const icm42670_sample_t& sample, void* user) {
  auto* state = static_cast<ImuPrintState*>(user);
  if (state == nullptr) return;
  const uint64_t previous_ns = state->last_sample_monotonic_ns;
  if (state->observed_samples == 0U) {
    state->first_sample_monotonic_ns = sample.sample_monotonic_ns;
  } else if (sample.sample_sequence <= state->last_sequence) {
    ++state->sequence_regression_count;
  } else {
    state->sequence_gap_count += sample.sample_sequence - state->last_sequence - 1U;
  }
  state->last_sample_monotonic_ns = sample.sample_monotonic_ns;
  state->last_sequence = sample.sample_sequence;
  ++state->observed_samples;
  if (sample.timestamp_source == ICM42670_TIMESTAMP_SOURCE_IRQ) ++state->irq_samples;
  if (sample.timestamp_source == ICM42670_TIMESTAMP_SOURCE_SERVICE) ++state->service_samples;

  // RAW endpoints measure acquisition independently of the MONOTONIC estimate.
  // Update the first burst's endpoint through its last sample, not its first.
  if ((sample.burst_flags & ICM42670_BURST_OK) != 0U &&
      (sample.burst_flags & ICM42670_BURST_STOP_TAIL) == 0U) {
    if (state->ordinary_bursts == 0U) {
      state->first_burst_raw_ns = sample.burst_complete_raw_ns;
      state->burst_generation = sample.session_generation;
    }
    if (state->ordinary_bursts == 0U || sample.evidence_id != state->last_evidence_id ||
        sample.session_generation != state->burst_generation) {
      ++state->ordinary_bursts;
    }
    if (state->ordinary_bursts == 1U) {
      state->first_burst_last_sequence = sample.sample_sequence;
    }
    state->last_evidence_id = sample.evidence_id;
    state->burst_generation = sample.session_generation;
    state->last_burst_raw_ns = sample.burst_complete_raw_ns;
    state->last_burst_last_sequence = sample.sample_sequence;
  }
  if (state->print_every_samples == 0U ||
      (state->observed_samples != 1U &&
       (state->observed_samples - 1U) % state->print_every_samples != 0U)) return;
  if (!state->output_available) {
    ++state->dropped_output_lines;
    return;
  }

  const double accel_norm =
      std::sqrt(sample.accel_mps2[0] * sample.accel_mps2[0] +
                sample.accel_mps2[1] * sample.accel_mps2[1] +
                sample.accel_mps2[2] * sample.accel_mps2[2]);
  std::array<char, PIPE_BUF> line{};
  std::size_t line_length = 0U;
  const auto append_to_line = [&](const char* format, auto... args) -> bool {
    const std::size_t remaining = line.size() - line_length;
    const int appended = std::snprintf(line.data() + line_length, remaining, format, args...);
    if (appended < 0 || static_cast<std::size_t>(appended) >= remaining) return false;
    line_length += static_cast<std::size_t>(appended);
    return true;
  };
  bool formatted = append_to_line(
      "*******************************IMU*******************************\n"
      "imu data:\n"
      "sample_seq=%llu\n"
      "sample_monotonic_ns=%llu\n"
      "timestamp_mode=%s fifo_tmst=%u sample_raw_ns=%llu timestamp_uncertainty_ns=%u\n"
      "fifo_header=%u fsync_flags=%u fsync_sequence=%llu fsync_raw_ns=%llu fsync_uncertainty_ns=%u fsync_delta_us=%u\n"
      "temp_c=%.6f accel_norm_mps2=%.6f\n"
      "accel_mps2=[%.6f, %.6f, %.6f]\n"
      "gyro_rps  =[%.6f, %.6f, %.6f]\n",
      static_cast<unsigned long long>(sample.sample_sequence),
      static_cast<unsigned long long>(sample.sample_monotonic_ns),
      sample.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST ? "tmst" : "host", sample.fifo_tmst,
      static_cast<unsigned long long>(sample.sample_raw_ns), sample.timestamp_uncertainty_ns,
      sample.fifo_header, sample.fsync_flags,
      static_cast<unsigned long long>(sample.fsync_sequence),
      static_cast<unsigned long long>(sample.fsync_raw_ns), sample.fsync_uncertainty_ns, sample.fsync_delta_us,
      sample.temperature_c, accel_norm,
      sample.accel_mps2[0], sample.accel_mps2[1], sample.accel_mps2[2],
      sample.gyro_rps[0], sample.gyro_rps[1], sample.gyro_rps[2]);
  if (formatted && state->print_metrics) {
    const double dt_ms = state->observed_samples < 2U ? 0.0 :
        (sample.sample_monotonic_ns >= previous_ns
             ? static_cast<double>(sample.sample_monotonic_ns - previous_ns)
             : -static_cast<double>(previous_ns - sample.sample_monotonic_ns)) / 1e6;
    const double observation_minus_sample_us =
        (sample.observation_monotonic_ns >= sample.sample_monotonic_ns
             ? static_cast<double>(sample.observation_monotonic_ns - sample.sample_monotonic_ns)
             : -static_cast<double>(sample.sample_monotonic_ns - sample.observation_monotonic_ns)) / 1e3;
    formatted = append_to_line(
        "metrics:\n"
        "session_generation=%u evidence_id=%llu timestamp_source=%s packet_flags=%u burst_flags=%u\n"
        "observation_monotonic_ns=%llu burst_complete_raw_ns=%llu estimated_period_ns=%llu\n"
        "observation_minus_sample_us=%.6f sample_monotonic_dt_ms=%.6f\n",
        sample.session_generation, static_cast<unsigned long long>(sample.evidence_id),
        sample.timestamp_source == ICM42670_TIMESTAMP_SOURCE_IRQ ? "IRQ" :
            sample.timestamp_source == ICM42670_TIMESTAMP_SOURCE_SERVICE ? "SERVICE" : "unknown",
        sample.packet_flags, sample.burst_flags,
        static_cast<unsigned long long>(sample.observation_monotonic_ns),
        static_cast<unsigned long long>(sample.burst_complete_raw_ns),
        static_cast<unsigned long long>(sample.estimated_period_ns),
        observation_minus_sample_us, dt_ms);
  }
  formatted = formatted && append_to_line(
      "%s", "*****************************************************************\n");
  if (!formatted) {
    ++state->dropped_output_lines;
    return;
  }
  const ssize_t written = ::write(state->output_fd, line.data(), line_length);
  if (written >= 0 && static_cast<std::size_t>(written) == line_length) return;
  ++state->dropped_output_lines;
  // Slow or closed sinks lose only logs. Never retry on the consumption path.
  if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
    state->output_available = false;
  }
}

}  // namespace robobaton_demo

#if !defined(RELEASE008_TESTING) && !defined(SENSOR_DEMO_NO_MAIN)
namespace {

using robobaton_demo::ImuConsumerOptions;
using robobaton_demo::ScopedNonblockingFd;

void SignalHandler(int) { robobaton_demo::g_imu_signal_stop = 1; }

std::string RequireValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

// 解析十进制 CLI 参数并拒绝 uint32 范围外输入，避免大整数截断成有效采样率或计数。
uint32_t ParseUint32Argument(const std::string& text, const char* name) {
  size_t parsed = 0U;
  const unsigned long value = std::stoul(text, &parsed, 10);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid unsigned integer for ") + name);
  }
  constexpr unsigned long kMaxUint32 = 0xffffffffUL;
  if (value > kMaxUint32) {
    throw std::out_of_range(std::string("out of range for ") + name);
  }
  return static_cast<uint32_t>(value);
}


ImuConsumerOptions ParseCommandLine(int argc, char** argv, uint32_t* print_rate_hz,
                                    bool* print_metrics) {
  if (print_rate_hz == nullptr || print_metrics == nullptr) {
    throw std::invalid_argument("CLI output pointer is null");
  }
  uint32_t requested_print_rate_hz = 0U;
  bool print_rate_was_set = false;
  bool print_metrics_enabled = false;
  ImuConsumerOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--sample-rate-hz") {
      options.sample_rate_hz =
          ParseUint32Argument(RequireValue(argc, argv, &index, "--sample-rate-hz"),
                              "--sample-rate-hz");
      if (options.sample_rate_hz != 25U && options.sample_rate_hz != 50U &&
          options.sample_rate_hz != 100U && options.sample_rate_hz != 200U &&
          options.sample_rate_hz != 400U && options.sample_rate_hz != 800U &&
          options.sample_rate_hz != 1600U) {
        throw std::invalid_argument("--sample-rate-hz must be 25, 50, 100, 200, 400, 800, or 1600");
      }
    } else if (argument == "--count") {
      options.count =
          ParseUint32Argument(RequireValue(argc, argv, &index, "--count"), "--count");
    } else if (argument == "--sample-drop-policy") {
      const std::string policy = RequireValue(argc, argv, &index, "--sample-drop-policy");
      if (policy != "strict" && policy != "allow-counted") {
        throw std::invalid_argument("--sample-drop-policy must be allow-counted or strict");
      }
      options.sample_drop_policy = policy == "strict"
          ? ICM42670_SAMPLE_DROP_POLICY_STRICT : ICM42670_SAMPLE_DROP_POLICY_ALLOW_COUNTED;
    } else if (argument == "--imu-timestamp-mode") {
      const auto mode = RequireValue(argc, argv, &index, "--imu-timestamp-mode");
      if (mode != "host" && mode != "tmst")
        throw std::invalid_argument("--imu-timestamp-mode must be host or tmst");
      options.timestamp_mode = mode == "tmst" ? ICM42670_TIMESTAMP_MODE_TMST : ICM42670_TIMESTAMP_MODE_HOST;
    } else if (argument == "--imu-fsync-diagnostics") {
      const auto value = RequireValue(argc, argv, &index, "--imu-fsync-diagnostics");
      if (value != "on" && value != "off")
        throw std::invalid_argument("--imu-fsync-diagnostics must be on or off");
      options.fsync_diagnostics = value == "on";
    } else if (argument == "--print-rate-hz") {
      requested_print_rate_hz =
          ParseUint32Argument(RequireValue(argc, argv, &index, "--print-rate-hz"),
                              "--print-rate-hz");
      print_rate_was_set = true;
    } else if (argument == "--print-metrics") {
      print_metrics_enabled = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: imu_reader_demo [options]:\n"
                << "  --sample-rate-hz <25|50|100|200|400|800|1600> IMU sample rate, default "
                << robobaton_demo::kDefaultImuSampleRateHz << "\n"
                << "  --count N Request STOP after N consumed samples, then drain the complete cohort; 0 runs until signal\n"
                << "  --sample-drop-policy <allow-counted|strict> Invalid six-axis payload policy, default allow-counted\n"
                << "  --imu-timestamp-mode <host|tmst> Kernel IIO timestamp mode, default host\n"
                << "  --imu-fsync-diagnostics <on|off> Independent FSYNC diagnostics, default on\n"
                << "  --print-rate-hz HZ Terminal sample output rate, default min(sample-rate-hz, 10); 0 disables samples, not final health\n"
                << "  --print-metrics Include metrics diagnostics section in each output record, default off\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  // 默认 10Hz 只限制终端日志，owner 仍消费全部 IMU 样本。
  *print_rate_hz = print_rate_was_set
                       ? requested_print_rate_hz
                       : std::min(options.sample_rate_hz,
                                  robobaton_demo::kDefaultImuPrintRateHz);
  // 完整解析后校验最终值，使参数顺序不影响结果。
  if (*print_rate_hz > options.sample_rate_hz) {
    throw std::invalid_argument("--print-rate-hz must not exceed --sample-rate-hz");
  }
  *print_metrics = print_metrics_enabled;
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "imu_reader_demo " << ROBOBATON_RELEASE_VERSION << "\n"
                << "libicm42670 " << icm42670_get_version() << " abi="
                << ICM42670_ABI_VERSION_MAJOR << "." << ICM42670_ABI_VERSION_MINOR << "\n";
      return 0;
    }
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    // stdout 关闭仅表示日志 sink 不可用，不能让 SIGPIPE 终止采集。
    signal(SIGPIPE, SIG_IGN);
    uint32_t print_rate_hz = 0U;
    bool print_metrics = false;
    ImuConsumerOptions options = ParseCommandLine(argc, argv, &print_rate_hz, &print_metrics);
    std::cout << "imu_timestamp_mode=" << (options.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST ? "tmst" : "host") << "\n";
    icm42670_runtime_health_t final_health = ICM42670_RUNTIME_HEALTH_INIT;
    options.final_health = &final_health;
    robobaton_demo::ImuPrintState state;
    state.print_every_samples =
        robobaton_demo::ImuPrintEverySamples(options.sample_rate_hz, print_rate_hz);
    state.print_metrics = print_metrics;
    // 非阻塞标志属于共享 OFD；RAII 将修改严格限制在采集窗口，
    // 设置失败时禁用 CLI 输出，析构覆盖正常返回和异常路径并恢复调用方状态。
    int result = 1;
    {
      ScopedNonblockingFd output_mode(state.output_fd);
      state.output_available = output_mode.active();
      constexpr char time_base[] =
          "TIME_BASE domain=monotonic sample_time=kernel_host_estimate "
          "observation_clock=monotonic burst_complete_clock=monotonic_raw "
          "clock_domains_not_interchangeable=yes\n";
      if (output_mode.active()) {
        const ssize_t written = ::write(state.output_fd, time_base, sizeof(time_base) - 1U);
        static_cast<void>(written);
      }
      result = robobaton_demo::RunIcmConsumer(options, robobaton_demo::PrintImuSample,
                                             &state);
      const double timestamp_hz = state.observed_samples > 1U &&
          state.last_sample_monotonic_ns > state.first_sample_monotonic_ns
          ? static_cast<double>(state.observed_samples - 1U) * 1e9 /
              static_cast<double>(state.last_sample_monotonic_ns - state.first_sample_monotonic_ns)
          : 0.0;
      const bool acquisition_valid = state.ordinary_bursts > 1U &&
          state.last_burst_raw_ns > state.first_burst_raw_ns &&
          state.last_burst_last_sequence > state.first_burst_last_sequence;
      const double acquisition_hz = acquisition_valid
          ? static_cast<double>(state.last_burst_last_sequence - state.first_burst_last_sequence) * 1e9 /
              static_cast<double>(state.last_burst_raw_ns - state.first_burst_raw_ns)
          : 0.0;
      std::array<char, PIPE_BUF> summary{};
      const int size = std::snprintf(summary.data(), summary.size(),
          "IMU_TIMING_SUMMARY samples=%llu set_odr_hz=%u timestamp_hz=%.6f "
          "acquisition_hz=%.6f acquisition_valid=%s acquisition_endpoints=ordinary_bursts_monotonic_raw "
          "ordinary_bursts=%llu sequence_gap_count=%llu sequence_regression_count=%llu "
          "irq_samples=%llu service_samples=%llu\n"
          "IMU_RUNTIME_HEALTH final=yes health_valid=%s session_generation=%u "
          "published_samples=%llu observed_samples=%llu output_dropped=%llu "
          "consumed_scans=%llu records_consumed=%llu records_acked=%llu last_packet_sequence=%llu "
          "irq_wakeup_count=%llu timer_service_count=%llu fatal_reason=%u fatal_errno=%u "
          "fifo_overflow_count=%u invalid_payload_count=%u scan_push_failure_count=%u "
          "evidence_full_count=%u timestamp_service_burst_count=%u tail_packet_count=%u "
          "unknown_hardware_tail=%u quiesce_errno=%u transport_complete=%u result=%d\n",
          static_cast<unsigned long long>(state.observed_samples), options.sample_rate_hz,
          timestamp_hz, acquisition_hz, acquisition_valid ? "yes" : "no",
          static_cast<unsigned long long>(state.ordinary_bursts),
          static_cast<unsigned long long>(state.sequence_gap_count),
          static_cast<unsigned long long>(state.sequence_regression_count),
          static_cast<unsigned long long>(state.irq_samples),
          static_cast<unsigned long long>(state.service_samples),
          final_health.session_generation != 0U ? "yes" : "no", final_health.session_generation,
          static_cast<unsigned long long>(final_health.published_samples),
          static_cast<unsigned long long>(state.observed_samples),
          static_cast<unsigned long long>(state.dropped_output_lines),
          static_cast<unsigned long long>(final_health.consumed_scans),
          static_cast<unsigned long long>(final_health.records_consumed),
          static_cast<unsigned long long>(final_health.records_acked),
          static_cast<unsigned long long>(final_health.last_packet_sequence),
          static_cast<unsigned long long>(final_health.irq_wakeup_count),
          static_cast<unsigned long long>(final_health.timer_service_count),
          final_health.fatal_reason, final_health.fatal_errno, final_health.fifo_overflow_count,
          final_health.invalid_payload_count, final_health.scan_push_failure_count,
          final_health.evidence_full_count, final_health.timestamp_service_burst_count,
          final_health.tail_packet_count, final_health.unknown_hardware_tail,
          final_health.quiesce_errno, final_health.transport_complete, result);
      // Final evidence follows the same best-effort, nonblocking sink contract as samples.
      if (output_mode.active() && size > 0 && static_cast<std::size_t>(size) < summary.size()) {
        if (::write(state.output_fd, summary.data(), static_cast<std::size_t>(size)) != size) {
          state.output_available = false;
        }
      }
    }
    if (result != 0) {
      std::cerr << "fatal: ICM consumer lifecycle failed\n";
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "fatal: unknown exception\n";
    return 1;
  }
}
#endif
