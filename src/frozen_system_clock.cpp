#include "frozen_system_clock.h"

#include <time.h>

#include <limits>
#include <ostream>
#include <stdexcept>

namespace robobaton_demo {
namespace {

constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;

uint64_t NegativeOffsetMagnitude(int64_t offset_ns) {
  const uint64_t max_positive =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (offset_ns == std::numeric_limits<int64_t>::min()) {
    return max_positive + 1ULL;
  }
  return static_cast<uint64_t>(-offset_ns);
}

}  // namespace

FrozenSystemClock::FrozenSystemClock() : FrozenSystemClock(ReadSystemClock, nullptr) {}

FrozenSystemClock::FrozenSystemClock(FrozenClockReadFn reader, void* user)
    : snapshot_(Capture(reader, user)) {}

int FrozenSystemClock::ReadSystemClock(FrozenClockId clock_id, uint64_t* timestamp_ns,
                                       void*) noexcept {
  if (timestamp_ns == nullptr) {
    return -1;
  }

  clockid_t native_clock = CLOCK_REALTIME;
  if (clock_id == FrozenClockId::kMonotonicRaw) {
    native_clock = CLOCK_MONOTONIC_RAW;
  }

  timespec value{};
  if (clock_gettime(native_clock, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0 || value.tv_nsec >= static_cast<long>(kNanosecondsPerSecond)) {
    return -1;
  }

  const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
  if (seconds > std::numeric_limits<uint64_t>::max() / kNanosecondsPerSecond) {
    return -1;
  }
  const uint64_t whole_seconds_ns = seconds * kNanosecondsPerSecond;
  const uint64_t subsecond_ns = static_cast<uint64_t>(value.tv_nsec);
  if (whole_seconds_ns > std::numeric_limits<uint64_t>::max() - subsecond_ns) {
    return -1;
  }

  *timestamp_ns = whole_seconds_ns + subsecond_ns;
  return 0;
}

uint64_t FrozenSystemClock::MapRawNs(uint64_t raw_timestamp_ns) const {
  const int64_t offset_ns = snapshot_.frozen_offset_ns;
  if (offset_ns >= 0) {
    const uint64_t positive_offset = static_cast<uint64_t>(offset_ns);
    if (raw_timestamp_ns > std::numeric_limits<uint64_t>::max() - positive_offset) {
      throw std::overflow_error("frozen system timestamp is outside uint64 range");
    }
    return raw_timestamp_ns + positive_offset;
  }

  const uint64_t negative_offset = NegativeOffsetMagnitude(offset_ns);
  if (raw_timestamp_ns < negative_offset) {
    throw std::overflow_error("frozen system timestamp is outside uint64 range");
  }
  return raw_timestamp_ns - negative_offset;
}

void FrozenSystemClock::PrintTimeBase(std::ostream& output) const {
  output << "TIME_BASE realtime_start_ns=" << snapshot_.realtime_start_ns
         << " monotonic_raw_start_ns=" << snapshot_.monotonic_raw_start_ns
         << " frozen_offset_ns=" << snapshot_.frozen_offset_ns << '\n';
  output.flush();
}


FrozenSystemClockSnapshot FrozenSystemClock::Capture(FrozenClockReadFn reader, void* user) {
  if (reader == nullptr) {
    throw std::invalid_argument("FrozenSystemClock reader is null");
  }

  // 启动锚点只读取一次 realtime 和 RAW，后续输出只使用冻结 offset 外推。
  FrozenSystemClockSnapshot snapshot{};
  if (reader(FrozenClockId::kRealtime, &snapshot.realtime_start_ns, user) != 0) {
    throw std::runtime_error("CLOCK_REALTIME read failed");
  }
  if (reader(FrozenClockId::kMonotonicRaw, &snapshot.monotonic_raw_start_ns, user) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW read failed");
  }
  snapshot.frozen_offset_ns = CheckedOffset(snapshot.realtime_start_ns,
                                            snapshot.monotonic_raw_start_ns);
  return snapshot;
}

int64_t FrozenSystemClock::CheckedOffset(uint64_t realtime_ns,
                                         uint64_t monotonic_raw_ns) {
  if (realtime_ns >= monotonic_raw_ns) {
    const uint64_t positive_offset = realtime_ns - monotonic_raw_ns;
    if (positive_offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::overflow_error("frozen realtime/raw offset is outside int64 range");
    }
    return static_cast<int64_t>(positive_offset);
  }

  const uint64_t negative_offset = monotonic_raw_ns - realtime_ns;
  const uint64_t max_negative_magnitude =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
  if (negative_offset > max_negative_magnitude) {
    throw std::overflow_error("frozen realtime/raw offset is outside int64 range");
  }
  if (negative_offset == max_negative_magnitude) {
    return std::numeric_limits<int64_t>::min();
  }
  return -static_cast<int64_t>(negative_offset);
}

}  // namespace robobaton_demo
