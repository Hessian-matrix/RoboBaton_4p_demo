#include "monotonic_raw_clock.h"

#include <time.h>

#include <limits>
#include <ostream>
#include <stdexcept>

namespace robobaton_demo {

static uint64_t ClockNowNs(clockid_t clock) {
  constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
  timespec value{};
  if (clock_gettime(clock, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0 || value.tv_nsec >= static_cast<long>(kNanosecondsPerSecond)) {
    throw std::runtime_error("clock_gettime failed");
  }
  const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
  const uint64_t nanos = static_cast<uint64_t>(value.tv_nsec);
  if (seconds > (std::numeric_limits<uint64_t>::max() - nanos) / kNanosecondsPerSecond) {
    throw std::overflow_error("clock is outside uint64 range");
  }
  return seconds * kNanosecondsPerSecond + nanos;
}

uint64_t MonotonicRawNowNs() { return ClockNowNs(CLOCK_MONOTONIC_RAW); }
uint64_t RealtimeNowNs() { return ClockNowNs(CLOCK_REALTIME); }

void PrintTimeBase(std::ostream& output) {
  output << "TIME_BASE domain=realtime realtime_start_ns=" << RealtimeNowNs()
         << '\n';
  output.flush();
}

}  // namespace robobaton_demo
