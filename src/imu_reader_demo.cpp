#include "icm42688_x5/icm42688_driver.h"

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_should_stop{false};

struct Options {
  uint32_t sample_rate_hz = 1000;
  uint32_t count = 0;
};

void SignalHandler(int /*signum*/) { g_should_stop.store(true); }

std::string RequireValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

Options ParseCommandLine(int argc, char** argv) {
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--sample-rate-hz") {
      options.sample_rate_hz =
          static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &i, "--sample-rate-hz")));
      if (options.sample_rate_hz == 0) {
        throw std::invalid_argument("--sample-rate-hz must be greater than 0");
      }
    } else if (arg == "--count") {
      options.count = static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &i, "--count")));
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: imu_reader_demo [options]\n"
                << "  --sample-rate-hz <hz>  IMU sample rate, default 1000\n"
                << "  --count <n>            Frames to print, 0 means forever\n"
                << "  -h, --help             Show this help\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }

  return options;
}

void PrintSample(const icm42688_x5::ImuSample& sample, double dt_ms) {
  const double accel_norm_mps2 =
      std::sqrt(sample.accel_x_mps2 * sample.accel_x_mps2 +
                sample.accel_y_mps2 * sample.accel_y_mps2 +
                sample.accel_z_mps2 * sample.accel_z_mps2);

  std::cout << std::fixed << std::setprecision(6)
            << "ts_ns=" << sample.host_timestamp_ns
            << " dt_ms=" << dt_ms
            << " temp_c=" << sample.temperature_c
            << " accel_mps2=[" << sample.accel_x_mps2 << ", " << sample.accel_y_mps2 << ", "
            << sample.accel_z_mps2 << "]"
            << " accel_norm_mps2=" << accel_norm_mps2
            << " gyro_rps=[" << sample.gyro_x_rps << ", " << sample.gyro_y_rps << ", "
            << sample.gyro_z_rps << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    const Options options = ParseCommandLine(argc, argv);

    icm42688_x5::DriverConfig config;
    config.sample_rate_hz = options.sample_rate_hz;
    config.read_mode = icm42688_x5::ReadMode::Fifo;
    config.fifo_watermark_samples = 8;

    icm42688_x5::Driver driver(config);
    std::atomic<uint32_t> emitted{0};
    uint64_t last_timestamp_ns = 0;

    driver.RegisterCallback([&](const icm42688_x5::ImuSample& sample) {
      const uint32_t current = emitted.load();
      if (options.count != 0 && current >= options.count) {
        return;
      }

      double dt_ms = 0.0;
      if (last_timestamp_ns != 0) {
        dt_ms = static_cast<double>(sample.host_timestamp_ns - last_timestamp_ns) / 1000000.0;
      }
      last_timestamp_ns = sample.host_timestamp_ns;

      // 2026-05-20 修改原因：回调运行在驱动采集线程中，demo 里只做轻量打印，避免阻塞采集。
      PrintSample(sample, dt_ms);
      emitted.store(current + 1);
    });

    std::cout << "Starting ICM-42688 demo at " << options.sample_rate_hz << " Hz\n";
    driver.Start();

    // 2026-05-20 修改原因：主线程只负责等待退出条件，数据读取由驱动内部线程完成。
    while (!g_should_stop.load()) {
      if (options.count != 0 && emitted.load() >= options.count) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    driver.Stop();
    std::cout << "Stopped, emitted_samples=" << emitted.load() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
