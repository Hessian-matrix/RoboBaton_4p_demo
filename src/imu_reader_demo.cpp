#include "cam_demo_common.h"

#include <signal.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
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
  // 首样本及固定抽样点才格式化并写终端，减少限速模式的同步 I/O。
  if (state->observed_samples != 1U &&
      (state->observed_samples - 1U) % state->print_every_samples != 0U) {
    return;
  }
  const double dt_ms = state->last_timestamp_ns == 0U
                           ? 0.0
                           : static_cast<double>(sample.host_timestamp_ns -
                                                 state->last_timestamp_ns) /
                                 1000000.0;
  state->last_timestamp_ns = sample.host_timestamp_ns;
  const double accel_norm =
      std::sqrt(sample.accel_mps2[0] * sample.accel_mps2[0] +
                sample.accel_mps2[1] * sample.accel_mps2[1] +
                sample.accel_mps2[2] * sample.accel_mps2[2]);
  std::cout << std::fixed << std::setprecision(6)
            << "ts_ns=" << sample.host_timestamp_ns << " dt_ms=" << dt_ms
            << " temp_c=" << sample.temperature_c
            << " accel_mps2=[" << sample.accel_mps2[0] << ", " << sample.accel_mps2[1]
            << ", " << sample.accel_mps2[2] << "] accel_norm_mps2=" << accel_norm
            << " gyro_rps=[" << sample.gyro_rps[0] << ", " << sample.gyro_rps[1]
            << ", " << sample.gyro_rps[2] << "]\n";
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
  // 未指定输出频率时逐样本输出；显式 0 保留禁用输出语义。
  *print_rate_hz = print_rate_was_set ? requested_print_rate_hz : options.sample_rate_hz;
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
    uint32_t print_rate_hz = 0U;
    const ImuConsumerOptions options = ParseCommandLine(argc, argv, &print_rate_hz);
    robobaton_demo::ImuPrintState state;
    state.print_every_samples =
        robobaton_demo::ImuPrintEverySamples(options.sample_rate_hz, print_rate_hz);
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
