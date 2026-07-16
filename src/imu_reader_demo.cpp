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

struct IcmCallbackContext {
  static constexpr std::size_t kPendingCapacity = 64U;

  std::atomic<bool> accepting{true};
  std::atomic<bool> callback_failed{false};
  std::atomic<uint32_t> emitted{0U};
  std::mutex pending_mutex;
  // 2026-07-16 修改原因：producer 会在一次 FIFO drain 中同步回调多个样本；
  // 固定容量环形队列保持原序且避免 callback 路径动态分配。
  std::array<icm42688_sample_t, kPendingCapacity> pending_samples{};
  std::size_t pending_head = 0U;
  std::size_t pending_size = 0U;
  ImuSampleObserver observer = nullptr;
  void* observer_user = nullptr;
};

// 2026-07-15 修改原因：producer callback thread 只能发布数据/首错；所有 C++
// observer 异常在此 catch-all，且 stop/destroy 始终由 RunIcmConsumer owner 执行。
void IcmCallback(const icm42688_sample_t* sample, void* user) noexcept {
  auto* context = static_cast<IcmCallbackContext*>(user);
  if (context == nullptr || !context->accepting.load(std::memory_order_acquire)) {
    return;
  }
  try {
    if (sample == nullptr || sample->struct_size != sizeof(*sample)) {
      // 2026-07-16 修改原因：callback 的无效输入直接首错关闭 admission，
      // 不构造可能分配内存的 C++ 异常对象。
      context->callback_failed.store(true, std::memory_order_release);
      context->accepting.store(false, std::memory_order_release);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(context->pending_mutex);
      // 2026-07-16 修改原因：在同一锁域内复核 admission 并执行 enqueue，
      // 防止 owner 关闭接收后，已等待锁的 callback 再写入队列。
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
    // 2026-07-15 修改原因：start 失败仍由 create owner finalizer 销毁非空 handle；
    // 不在同一进程重试/restart 半初始化 generation。
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
        // 2026-07-16 修改原因：owner 每次只取最老样本；observer 在解锁后运行，
        // 避免打印或用户逻辑阻塞 producer callback enqueue。
        sample = context.pending_samples[context.pending_head];
        context.pending_head =
            (context.pending_head + 1U) % IcmCallbackContext::kPendingCapacity;
        --context.pending_size;
        has_sample = true;
      }
    }
    if (has_sample) {
      try {
        // 2026-07-15 修改原因：打印/用户 observer 在 lifecycle owner 线程运行；
        // producer callback 只复制发布 sample，绝不执行潜在阻塞 I/O。
        if (context.observer != nullptr) {
          context.observer(sample, context.observer_user);
        }
        context.emitted.fetch_add(1U, std::memory_order_acq_rel);
      } catch (...) {
        context.callback_failed.store(true, std::memory_order_release);
        context.accepting.store(false, std::memory_order_release);
        break;
      }
    }
    if (options.count != 0U &&
        context.emitted.load(std::memory_order_acquire) >= options.count) {
      break;
    }
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

ImuConsumerOptions ParseCommandLine(int argc, char** argv) {
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
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: imu_reader_demo [--sample-rate-hz HZ] [--count N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  return options;
}

struct PrintState {
  uint64_t last_timestamp_ns = 0U;
};

void PrintSample(const icm42688_sample_t& sample, void* user) {
  auto* state = static_cast<PrintState*>(user);
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
            << " temp_c=" << sample.temperature_c << " accel_norm_mps2=" << accel_norm
            << " gyro_rps=[" << sample.gyro_rps[0] << ", " << sample.gyro_rps[1]
            << ", " << sample.gyro_rps[2] << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    const ImuConsumerOptions options = ParseCommandLine(argc, argv);
    PrintState state;
    const int result = robobaton_demo::RunIcmConsumer(options, PrintSample, &state);
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
