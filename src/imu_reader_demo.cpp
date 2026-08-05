#include "cam_demo_common.h"

#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include "icm42688_driver.h"
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
  static constexpr std::size_t kPendingCapacity = 64U;

  std::atomic<bool> accepting{true};
  std::atomic<bool> callback_failed{false};
  std::atomic<uint32_t> emitted{0U};
  std::mutex pending_mutex;
  // 固定容量 FIFO 保持 burst 顺序并避免 callback 路径动态分配。
  std::array<icm42688_sample_t, kPendingCapacity> pending_samples{};
  std::size_t pending_head = 0U;
  std::size_t pending_size = 0U;
  ImuSampleObserver observer = nullptr;
  void* observer_user = nullptr;
};

// callback 只发布数据或首错；stop/destroy 由 lifecycle owner 执行。
void IcmCallback(const icm42688_sample_t* sample, void* user) noexcept {
  auto* context = static_cast<IcmCallbackContext*>(user);
  if (context == nullptr || !context->accepting.load(std::memory_order_acquire)) {
    return;
  }
  try {
    if (sample == nullptr || sample->struct_size != sizeof(*sample)) {
      // 无效输入直接关闭 admission，避免在 producer 线程分配异常对象。
      context->callback_failed.store(true, std::memory_order_release);
      context->accepting.store(false, std::memory_order_release);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(context->pending_mutex);
      // 在同一锁域复核 admission 并 enqueue。
      if (!context->accepting.load(std::memory_order_acquire)) {
        return;
      }
      if (context->pending_size == IcmCallbackContext::kPendingCapacity) {
        // 容量耗尽不能覆盖或丢弃旧样本：关闭 admission，让 owner 返回非零。
        context->callback_failed.store(true, std::memory_order_release);
        context->accepting.store(false, std::memory_order_release);
        return;
      }
      const std::size_t tail =
          (context->pending_head + context->pending_size) %
          IcmCallbackContext::kPendingCapacity;
      context->pending_samples[tail] = *sample;
      ++context->pending_size;
    }
  } catch (...) {
    context->callback_failed.store(true, std::memory_order_release);
    context->accepting.store(false, std::memory_order_release);
  }
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
  if (options.sample_rate_hz == 0U) {
    return 1;
  }

  icm42688_config_t config = ICM42688_CONFIG_INIT;
  config.sample_rate_hz = options.sample_rate_hz;
  config.fifo_watermark_samples = 1U;
  config.read_mode = ICM42688_READ_MODE_SENSOR_TIMESTAMP_FIFO;
  // 策略通过 ABI v2 reserved[0] 传递，不能扩展公开 config 布局。
  config.reserved[ICM42688_CONFIG_SAMPLE_DROP_POLICY_INDEX] = options.sample_drop_policy;

  IcmCallbackContext context;
  context.observer = observer;
  context.observer_user = observer_user;
  icm42688_handle_t* handle = nullptr;

  int result = icm42688_create(&config, &handle);
  if (result != ICM42688_STATUS_OK || handle == nullptr) {
    if (handle != nullptr) {
      icm42688_destroy(handle);
    }
    return 1;
  }

  result = icm42688_set_callback(handle, IcmCallback, &context);
  if (result != ICM42688_STATUS_OK) {
    context.accepting.store(false, std::memory_order_release);
    icm42688_destroy(handle);
    return 1;
  }

  result = icm42688_start(handle);
  if (result != ICM42688_STATUS_OK) {
    context.accepting.store(false, std::memory_order_release);
    // start 失败后仍由 create owner 销毁非空 handle。
    icm42688_destroy(handle);
    return 1;
  }

  while (g_imu_signal_stop == 0 &&
         (options.stop_requested == nullptr ||
          !options.stop_requested->load(std::memory_order_acquire)) &&
         !context.callback_failed.load(std::memory_order_acquire)) {
    icm42688_sample_t sample{};
    bool has_sample = false;
    {
      std::lock_guard<std::mutex> lock(context.pending_mutex);
      if (context.pending_size != 0U) {
        // 按 FIFO 取最老样本，并在解锁后执行 observer。
        sample = context.pending_samples[context.pending_head];
        context.pending_head =
            (context.pending_head + 1U) % IcmCallbackContext::kPendingCapacity;
        --context.pending_size;
        has_sample = true;
      }
    }
    // 队列有积压时立即继续 drain，仅空队列进入短等待。
    if (has_sample) {
      try {
        if (options.system_clock != nullptr) {
          sample.host_timestamp_ns = options.system_clock->MapRawNs(sample.host_timestamp_ns);
          sample.sample_timestamp_ns = options.system_clock->MapRawNs(sample.sample_timestamp_ns);
        }
        // observer 在 owner 线程运行，producer callback 不执行阻塞 I/O。
        if (context.observer != nullptr) {
          context.observer(sample, context.observer_user);
        }
        context.emitted.fetch_add(1U, std::memory_order_acq_rel);
      } catch (...) {
        context.callback_failed.store(true, std::memory_order_release);
        context.accepting.store(false, std::memory_order_release);
        break;
      }
      if (options.count != 0U &&
          context.emitted.load(std::memory_order_acquire) >= options.count) {
        break;
      }
      continue;
    }
    const bool owner_stop_requested =
        g_imu_signal_stop != 0 ||
        (options.stop_requested != nullptr &&
         options.stop_requested->load(std::memory_order_acquire));
    if (!owner_stop_requested && icm42688_is_running(handle) == 0) {
      // producer 在未达 count 且未收到 owner 停止请求前退出，只能在空队列边界确认已 drain 后 fail-closed。
      context.callback_failed.store(true, std::memory_order_release);
      context.accepting.store(false, std::memory_order_release);
      break;
    }
#ifdef RELEASE008_TESTING
    // 测试计数只位于空队列分支，用于验证积压期间不等待。
    g_idle_wait_count.fetch_add(1U, std::memory_order_relaxed);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  context.accepting.store(false, std::memory_order_release);
  // context 栈对象必须覆盖 blocking stop：fake/producer 可在 stop join 前完成已进入 callback，
  // 但 admission 已关闭，因此不会再调用上层 observer。
  const int stop_result = icm42688_stop(handle);
  const bool failed = context.callback_failed.load(std::memory_order_acquire) ||
                      stop_result != ICM42688_STATUS_OK;
  icm42688_destroy(handle);
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

void PrintImuSample(const icm42688_sample_t& sample, void* user) {
  auto* state = static_cast<ImuPrintState*>(user);
  if (state == nullptr) {
    return;
  }
  ++state->observed_samples;
  if (state->print_every_samples == 0U) {
    return;
  }
  // 仅在固定抽样点尝试输出；输出端失效后仍完整消费样本。
  if (state->observed_samples != 1U &&
      (state->observed_samples - 1U) % state->print_every_samples != 0U) {
    return;
  }
  if (!state->output_available) {
    ++state->dropped_output_lines;
    return;
  }

  const uint64_t timestamp_ns = sample.sample_timestamp_ns;
  const double accel_norm =
      std::sqrt(sample.accel_mps2[0] * sample.accel_mps2[0] +
                sample.accel_mps2[1] * sample.accel_mps2[1] +
                sample.accel_mps2[2] * sample.accel_mps2[2]);

  // 每条终端记录保持在 PIPE_BUF 内，并通过一次 write 写出完整多行块；
  // 慢 sink 或关闭的输出端只影响日志记录，不阻塞采集 owner。
  std::array<char, PIPE_BUF> line;
  const unsigned long long ts_ns = static_cast<unsigned long long>(timestamp_ns);
  std::size_t line_length = 0U;
  const auto append_to_line = [&](const char* format, auto... args) -> bool {
    const std::size_t remaining = line.size() - line_length;
    const int appended = std::snprintf(line.data() + line_length, remaining, format,
                                       args...);
    if (appended < 0 || static_cast<std::size_t>(appended) >= remaining) {
      return false;
    }
    line_length += static_cast<std::size_t>(appended);
    return true;
  };

  // 必选 data 段先写入；metrics 诊断段只在 CLI 开关打开时追加，最终分割符始终追加。
  bool formatted = append_to_line(
      "*******************************IMU*******************************\n"
      "imu data:\n"
      "sample_seq=%llu\n"
      "ts_ns=%llu\n"
      "temp_c=%.6f accel_norm_mps2=%.6f\n"
      "accel_mps2=[%.6f, %.6f, %.6f]\n"
      "gyro_rps  =[%.6f, %.6f, %.6f]\n",
      static_cast<unsigned long long>(sample.sample_sequence), ts_ns,
      sample.temperature_c, accel_norm,
      sample.accel_mps2[0], sample.accel_mps2[1], sample.accel_mps2[2],
      sample.gyro_rps[0], sample.gyro_rps[1], sample.gyro_rps[2]);
  if (formatted && state->print_metrics) {
    const unsigned long long host_ns =
        static_cast<unsigned long long>(sample.host_timestamp_ns);
    const double dt_ms = state->last_timestamp_ns == 0U
                             ? 0.0
                             : static_cast<double>(timestamp_ns - state->last_timestamp_ns) /
                                   1000000.0;
    const double host_ts_gap_ms = host_ns >= ts_ns
                                      ? static_cast<double>(host_ns - ts_ns) / 1000000.0
                                      : -static_cast<double>(ts_ns - host_ns) / 1000000.0;
    formatted = append_to_line(
        "metrics:\n"
        "host_ts_ns=%llu\n"
        "host_ts_gap_ms=%.6f dt_ms=%.6f uncertainty_us=%u\n"
        "gpio_gap_count=%u fifo_overflow_count=%u mapper_failure_count=%u\n",
        host_ns, host_ts_gap_ms, dt_ms,
        sample.timestamp_uncertainty_us, sample.gpio_event_gap_count,
        sample.fifo_overflow_count, sample.mapper_failure_count);
  }
  formatted = formatted && append_to_line(
      "%s", "*****************************************************************\n");
  if (!formatted) {
    ++state->dropped_output_lines;
    return;
  }

  const ssize_t written = ::write(state->output_fd, line.data(), line_length);
  if (written >= 0 && static_cast<std::size_t>(written) == line_length) {
    state->last_timestamp_ns = timestamp_ns;
    return;
  }

  // 慢 sink 或信号中断只丢当前日志，owner 绝不重试；
  // partial write 与 EPIPE/永久 fd 错误均关闭后续日志，避免连续产生截断行。
  ++state->dropped_output_lines;
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
      if (options.sample_rate_hz == 0U) {
        throw std::invalid_argument("--sample-rate-hz must be positive");
      }
    } else if (argument == "--count") {
      options.count =
          ParseUint32Argument(RequireValue(argc, argv, &index, "--count"), "--count");
    } else if (argument == "--print-rate-hz") {
      requested_print_rate_hz =
          ParseUint32Argument(RequireValue(argc, argv, &index, "--print-rate-hz"),
                              "--print-rate-hz");
      print_rate_was_set = true;
    } else if (argument == "--print-metrics") {
      print_metrics_enabled = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: imu_reader_demo [options]:\n"
                << "  --sample-rate-hz <25|50|100|200|500|1000|2000> IMU sample rate, default "
                << robobaton_demo::kDefaultImuSampleRateHz << "\n"
                << "  --count N Number of IMU samples to consume before exit, default 0 (run until signal)\n"
                << "  --print-rate-hz HZ Terminal output rate, default min(sample-rate-hz, 10); 0 disables output\n"
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
                << "libicm42688 " << icm42688_get_version() << " abi="
                << ICM42688_ABI_VERSION_MAJOR << "." << ICM42688_ABI_VERSION_MINOR << "\n";
      return 0;
    }
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    // stdout 关闭仅表示日志 sink 不可用，不能让 SIGPIPE 终止采集。
    signal(SIGPIPE, SIG_IGN);
    uint32_t print_rate_hz = 0U;
    bool print_metrics = false;
    ImuConsumerOptions options = ParseCommandLine(argc, argv, &print_rate_hz, &print_metrics);
    robobaton_demo::FrozenSystemClock system_clock;
    system_clock.PrintTimeBase(std::cout);
    options.system_clock = &system_clock;
    robobaton_demo::ImuPrintState state;
    state.print_every_samples =
        robobaton_demo::ImuPrintEverySamples(options.sample_rate_hz, print_rate_hz);
    state.print_metrics = print_metrics;
    // 非阻塞标志属于共享 OFD；RAII 将修改严格限制在采集窗口，
    // 设置失败时禁用 CLI 输出，析构覆盖正常返回和异常路径并恢复调用方状态。
    ScopedNonblockingFd output_mode(state.output_fd);
    state.output_available = output_mode.active();
    const int result = robobaton_demo::RunIcmConsumer(options, robobaton_demo::PrintImuSample,
                                                      &state);
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
