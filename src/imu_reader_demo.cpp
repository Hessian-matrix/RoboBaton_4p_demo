#include "cam_demo_common.h"

#include <fcntl.h>
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
#endif

int RunIcmConsumer(const ImuConsumerOptions& options, ImuSampleObserver observer,
                   void* observer_user) {
  if (options.sample_rate_hz == 0U) {
    return 1;
  }

  icm42688_config_t config = ICM42688_CONFIG_INIT;
  config.sample_rate_hz = options.sample_rate_hz;
  config.fifo_watermark_samples = 8U;
  config.read_mode = ICM42688_READ_MODE_FIFO;

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

  const double dt_ms = state->last_timestamp_ns == 0U
                           ? 0.0
                           : static_cast<double>(sample.host_timestamp_ns -
                                                 state->last_timestamp_ns) /
                                 1000000.0;
  const double accel_norm =
      std::sqrt(sample.accel_mps2[0] * sample.accel_mps2[0] +
                sample.accel_mps2[1] * sample.accel_mps2[1] +
                sample.accel_mps2[2] * sample.accel_mps2[2]);

  // 固定栈缓冲保证单行小于 PIPE_BUF，一次 write 对 pipe 保持原子性，
  // 且避免 iostream 在 O_NONBLOCK fd 上产生不可控的缓冲、重试或部分写行为。
  std::array<char, PIPE_BUF> line;
  const int line_length = std::snprintf(
      line.data(), line.size(),
      "ts_ns=%llu dt_ms=%.6f temp_c=%.6f accel_mps2=[%.6f, %.6f, %.6f] "
      "accel_norm_mps2=%.6f gyro_rps=[%.6f, %.6f, %.6f]\n",
      static_cast<unsigned long long>(sample.host_timestamp_ns), dt_ms,
      sample.temperature_c, sample.accel_mps2[0], sample.accel_mps2[1],
      sample.accel_mps2[2], accel_norm, sample.gyro_rps[0], sample.gyro_rps[1],
      sample.gyro_rps[2]);
  if (line_length < 0 || static_cast<size_t>(line_length) >= line.size()) {
    ++state->dropped_output_lines;
    return;
  }

  const ssize_t written =
      ::write(state->output_fd, line.data(), static_cast<size_t>(line_length));
  if (written == line_length) {
    state->last_timestamp_ns = sample.host_timestamp_ns;
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

#ifndef RELEASE008_TESTING
namespace {

using robobaton_demo::ImuConsumerOptions;

void SignalHandler(int) { robobaton_demo::g_imu_signal_stop = 1; }

std::string RequireValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

class ScopedNonblockingFd final {
 public:
  explicit ScopedNonblockingFd(int fd) : fd_(fd), original_flags_(fcntl(fd, F_GETFL, 0)) {
    // 仅成功设置非阻塞位后才 armed；失败时调用方必须禁用输出。
    if (original_flags_ >= 0 &&
        fcntl(fd_, F_SETFL, original_flags_ | O_NONBLOCK) == 0) {
      armed_ = true;
    }
  }

  ScopedNonblockingFd(const ScopedNonblockingFd&) = delete;
  ScopedNonblockingFd& operator=(const ScopedNonblockingFd&) = delete;

  ~ScopedNonblockingFd() {
    if (armed_ && fcntl(fd_, F_SETFL, original_flags_) < 0) {
      // 析构路径不得抛异常，但恢复失败必须留下明确诊断。
      constexpr char kWarning[] =
          "warning: failed to restore stdout file status flags\n";
      const ssize_t warning_result =
          ::write(STDERR_FILENO, kWarning, sizeof(kWarning) - 1U);
      static_cast<void>(warning_result);
    }
  }

  bool active() const { return armed_; }

 private:
  int fd_ = -1;
  int original_flags_ = -1;
  bool armed_ = false;
};

ImuConsumerOptions ParseCommandLine(int argc, char** argv, uint32_t* print_rate_hz) {
  if (print_rate_hz == nullptr) {
    throw std::invalid_argument("print_rate_hz output is null");
  }
  uint32_t requested_print_rate_hz = 0U;
  bool print_rate_was_set = false;
  ImuConsumerOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--sample-rate-hz") {
      options.sample_rate_hz = static_cast<uint32_t>(
          std::stoul(RequireValue(argc, argv, &index, "--sample-rate-hz")));
      if (options.sample_rate_hz == 0U) {
        throw std::invalid_argument("--sample-rate-hz must be positive");
      }
    } else if (argument == "--count") {
      options.count =
          static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &index, "--count")));
    } else if (argument == "--print-rate-hz") {
      requested_print_rate_hz = static_cast<uint32_t>(
          std::stoul(RequireValue(argc, argv, &index, "--print-rate-hz")));
      print_rate_was_set = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: imu_reader_demo [--sample-rate-hz HZ] [--count N] "
                   "[--print-rate-hz HZ]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  // 默认 10Hz 只限制终端日志，owner 仍消费全部 IMU 样本。
  constexpr uint32_t kDefaultPrintRateHz = 10U;
  *print_rate_hz = print_rate_was_set
                       ? requested_print_rate_hz
                       : std::min(options.sample_rate_hz, kDefaultPrintRateHz);
  // 完整解析后校验最终值，使参数顺序不影响结果。
  if (*print_rate_hz > options.sample_rate_hz) {
    throw std::invalid_argument("--print-rate-hz must not exceed --sample-rate-hz");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    // stdout 关闭仅表示日志 sink 不可用，不能让 SIGPIPE 终止采集。
    signal(SIGPIPE, SIG_IGN);
    uint32_t print_rate_hz = 0U;
    const ImuConsumerOptions options = ParseCommandLine(argc, argv, &print_rate_hz);
    robobaton_demo::ImuPrintState state;
    state.print_every_samples =
        robobaton_demo::ImuPrintEverySamples(options.sample_rate_hz, print_rate_hz);
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
