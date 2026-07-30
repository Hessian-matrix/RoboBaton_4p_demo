#pragma once

#include <cstdint>
#include <iosfwd>

namespace robobaton_demo {

enum class FrozenClockId {
  kRealtime,
  kMonotonicRaw,
};

using FrozenClockReadFn = int (*)(FrozenClockId clock_id, uint64_t* timestamp_ns,
                                  void* user);

struct FrozenSystemClockSnapshot {
  uint64_t realtime_start_ns = 0U;
  uint64_t monotonic_raw_start_ns = 0U;
  int64_t frozen_offset_ns = 0;
};

class FrozenSystemClock final {
 public:
  FrozenSystemClock();
  FrozenSystemClock(FrozenClockReadFn reader, void* user);

  static int ReadSystemClock(FrozenClockId clock_id, uint64_t* timestamp_ns,
                             void* user) noexcept;

  uint64_t MapRawNs(uint64_t raw_timestamp_ns) const;

  const FrozenSystemClockSnapshot& snapshot() const noexcept { return snapshot_; }
  uint64_t realtime_start_ns() const noexcept { return snapshot_.realtime_start_ns; }
  uint64_t monotonic_raw_start_ns() const noexcept {
    return snapshot_.monotonic_raw_start_ns;
  }
  int64_t frozen_offset_ns() const noexcept { return snapshot_.frozen_offset_ns; }

  void PrintTimeBase(std::ostream& output) const;

 private:
  static FrozenSystemClockSnapshot Capture(FrozenClockReadFn reader, void* user);
  static int64_t CheckedOffset(uint64_t realtime_ns, uint64_t monotonic_raw_ns);

  FrozenSystemClockSnapshot snapshot_{};
};

}  // namespace robobaton_demo
