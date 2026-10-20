#pragma once

#include <cstdint>
#include <iosfwd>

namespace robobaton_demo {

// RAW is a local boot-relative clock, not Unix time or ROS simulation time.
uint64_t MonotonicRawNowNs();
// Public timestamps use Unix system time; wall-clock steps are preserved.
uint64_t RealtimeNowNs();
void PrintTimeBase(std::ostream& output);

}  // namespace robobaton_demo
