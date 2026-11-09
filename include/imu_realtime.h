#pragma once

#include "icm42670_driver.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <time.h>

namespace robobaton_demo {
struct OutputImuSample {
  icm42670_sample_t native{};
  uint64_t realtime_ns = 0;
  uint64_t bridge_monotonic_ns = 0;
  uint64_t bridge_realtime_ns = 0;
  uint64_t bridge_uncertainty_ns = 0;
  uint64_t bridge_raw_ns = 0;
};

// Output-only mapping. The original kernel sample and RAW burst observation
// are never rewritten. A wall-clock step is visible at the next burst; the
// bracket bounds observation latency, not physical sample-time precision.
class ImuRealtimeMapper {
 public:
  OutputImuSample Map(const icm42670_sample_t& sample) {
    if (sample.timestamp_mode != ICM42670_TIMESTAMP_MODE_HOST &&
        sample.timestamp_mode != ICM42670_TIMESTAMP_MODE_TMST)
      throw std::runtime_error("invalid native IMU timestamp mode");
    const bool raw = sample.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST;
    const auto sample_ns = raw ? sample.sample_raw_ns : sample.sample_monotonic_ns;
    const clockid_t clock = raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
    if (sample.struct_size != sizeof(sample) || !sample.session_generation ||
        !sample.evidence_id || !sample.sample_monotonic_ns || !sample_ns)
      throw std::runtime_error("invalid native IMU identity");
    if (generation_ != sample.session_generation || evidence_ != sample.evidence_id ||
        mode_ != sample.timestamp_mode) {
      bool valid = false;
      for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto before = Now(clock);
        const auto real = Now(CLOCK_REALTIME);
        const auto after = Now(clock);
        if (after < before) throw std::runtime_error("IMU native clock regressed");
        if (after-before > 200000) continue;
        monotonic_ = before+(after-before)/2;
        realtime_ = real;
        uncertainty_ = (after-before+1)/2;
        valid = true;
        break;
      }
      if (!valid) throw std::runtime_error("IMU realtime mapping bracket exceeded");
      generation_ = sample.session_generation; evidence_ = sample.evidence_id;
      mode_ = sample.timestamp_mode;
    }
    uint64_t result;
    if (sample_ns >= monotonic_) {
      const auto delta = sample_ns-monotonic_;
      if (delta > UINT64_MAX-realtime_) throw std::overflow_error("IMU realtime overflow");
      result = realtime_+delta;
    } else {
      const auto delta = monotonic_-sample_ns;
      if (delta >= realtime_) throw std::overflow_error("IMU realtime underflow");
      result = realtime_-delta;
    }
    return {sample, result, raw ? 0U : monotonic_, realtime_, uncertainty_, raw ? monotonic_ : 0U};
  }
 private:
  static uint64_t Now(clockid_t clock) {
    timespec t{};
    if (clock_gettime(clock, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L ||
        static_cast<uint64_t>(t.tv_sec) > (UINT64_MAX-static_cast<uint64_t>(t.tv_nsec))/1000000000ULL)
      throw std::runtime_error("IMU output clock failed");
    return static_cast<uint64_t>(t.tv_sec)*1000000000ULL+static_cast<uint64_t>(t.tv_nsec);
  }
  uint32_t generation_ = 0;
  uint32_t mode_ = ICM42670_TIMESTAMP_MODE_HOST;
  uint64_t evidence_ = 0, monotonic_ = 0, realtime_ = 0, uncertainty_ = 0;
};

inline size_t FormatImuTiming(const OutputImuSample& s, char* out, size_t capacity) {
  const auto& n = s.native;
  const bool raw = n.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST;
  const char* clock = raw ? "CLOCK_MONOTONIC_RAW" : "CLOCK_MONOTONIC";
  const int size = std::snprintf(out, capacity,
      "{\"schema\":\"robobaton_imu_timing_v3\",\"sample_clock\":\"%s\","
      "\"output_clock\":\"CLOCK_REALTIME\",\"burst_complete_clock\":\"CLOCK_MONOTONIC_RAW\","
      "\"generation\":%u,\"sample_sequence\":%llu,\"evidence_id\":%llu,\"burst_flags\":%u,"
      "\"sample_monotonic_ns\":%llu,\"sample_realtime_ns\":%llu,\"burst_complete_raw_ns\":%llu,"
      "\"observation_monotonic_ns\":%llu,\"estimated_period_ns\":%llu,\"timestamp_source\":%u,"
      "\"packet_flags\":%u,\"timestamp_mode\":\"%s\",\"fifo_tmst\":%u,"
      "\"sample_raw_ns\":%llu,\"timestamp_uncertainty_ns\":%u,\"bridge_clock\":\"%s\","
      "\"bridge_monotonic_ns\":%llu,\"bridge_raw_ns\":%llu,\"bridge_realtime_ns\":%llu,\"bridge_uncertainty_ns\":%llu,"
      "\"fifo_header\":%u,\"fsync_flags\":%u,\"fsync_sequence\":%llu,\"fsync_raw_ns\":%llu,"
      "\"fsync_uncertainty_ns\":%u,\"fsync_delta_us\":%u}",
      clock, n.session_generation, static_cast<unsigned long long>(n.sample_sequence),
      static_cast<unsigned long long>(n.evidence_id), n.burst_flags,
      static_cast<unsigned long long>(n.sample_monotonic_ns), static_cast<unsigned long long>(s.realtime_ns),
      static_cast<unsigned long long>(n.burst_complete_raw_ns), static_cast<unsigned long long>(n.observation_monotonic_ns),
      static_cast<unsigned long long>(n.estimated_period_ns), n.timestamp_source, n.packet_flags,
      raw ? "tmst" : "host", n.fifo_tmst, static_cast<unsigned long long>(n.sample_raw_ns),
      n.timestamp_uncertainty_ns, clock,
      static_cast<unsigned long long>(s.bridge_monotonic_ns), static_cast<unsigned long long>(s.bridge_raw_ns),
      static_cast<unsigned long long>(s.bridge_realtime_ns),
      static_cast<unsigned long long>(s.bridge_uncertainty_ns), n.fifo_header, n.fsync_flags,
      static_cast<unsigned long long>(n.fsync_sequence), static_cast<unsigned long long>(n.fsync_raw_ns),
      n.fsync_uncertainty_ns, n.fsync_delta_us);
  if (size < 0 || static_cast<size_t>(size) >= capacity) throw std::runtime_error("IMU timing metadata too large");
  return static_cast<size_t>(size);
}
}  // namespace robobaton_demo
