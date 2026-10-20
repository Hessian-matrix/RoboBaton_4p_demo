#pragma once
#include "realtime_mutex.h"
#include "icm42670_driver.h"
#include "sc132camera.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace robobaton_demo {
struct FsyncAssociationRecord {
  // 1 matched, 2 no hardware event/group, 3 ambiguous; never nearest-neighbor.
  uint32_t status = 0, camera_generation = 0, camera_mask = 0;
  uint64_t group_id = 0, group_timestamp_ns = 0, group_raw_ns = 0, group_uncertainty_ns = 0;
  std::array<uint32_t, 4> frame_ids{};
  std::array<uint64_t, 4> camera_timestamps_ns{};
  icm42670_fsync_event_t event{};
};
inline size_t FormatFsyncAssociation(const FsyncAssociationRecord& r, char* out, size_t capacity) {
  const auto& e = r.event;
  const int size = std::snprintf(out, capacity,
    "{\"schema\":\"robobaton_fsync_v1\",\"status\":%u,\"camera_generation\":%u,\"camera_mask\":%u,"
    "\"group_id\":%llu,\"group_timestamp_ns\":%llu,\"group_raw_ns\":%llu,\"group_uncertainty_ns\":%llu,"
    "\"frame_ids\":[%u,%u,%u,%u],\"camera_timestamps_ns\":[%llu,%llu,%llu,%llu],"
    "\"imu_generation\":%u,\"fsync_sequence\":%llu,\"sample_sequence\":%llu,"
    "\"sample_timestamp_ns\":%llu,\"fsync_timestamp_ns\":%llu,\"sample_raw_ns\":%llu,\"fsync_raw_ns\":%llu,"
    "\"fsync_to_sample_ns\":%llu,\"uncertainty_ns\":%llu}",
    r.status,r.camera_generation,r.camera_mask,(unsigned long long)r.group_id,
    (unsigned long long)r.group_timestamp_ns,(unsigned long long)r.group_raw_ns,
    (unsigned long long)r.group_uncertainty_ns,r.frame_ids[0],r.frame_ids[1],r.frame_ids[2],r.frame_ids[3],
    (unsigned long long)r.camera_timestamps_ns[0],(unsigned long long)r.camera_timestamps_ns[1],
    (unsigned long long)r.camera_timestamps_ns[2],(unsigned long long)r.camera_timestamps_ns[3],
    e.session_generation,(unsigned long long)e.fsync_sequence,(unsigned long long)e.sample_sequence,
    (unsigned long long)e.sample_timestamp_ns,(unsigned long long)e.fsync_timestamp_ns,
    (unsigned long long)e.sample_raw_timestamp_ns,(unsigned long long)e.fsync_raw_timestamp_ns,
    (unsigned long long)e.fsync_to_sample_ns,(unsigned long long)e.uncertainty_ns);
  if (size < 0 || static_cast<size_t>(size) >= capacity) throw std::runtime_error("FSYNC metadata buffer too small");
  return static_cast<size_t>(size);
}
class FsyncAssociation {
 public:
  using Sink = void (*)(const FsyncAssociationRecord&, void*);
  FsyncAssociation(Sink sink, void* user) : sink_(sink), user_(user) {}
  void ObserveGroup(const sc132_frame_set_t& set) {
    if (set.reserved[SC132_GROUP_TIMING_VALID] != SC132_GROUP_TIMING_MAGIC) return;
    std::lock_guard<RealtimeMutex> lock(mutex_);
    const uint32_t generation = set.reserved[SC132_GROUP_GENERATION];
    if (camera_generation_ && generation != camera_generation_) throw std::runtime_error("camera restarted during FSYNC association");
    camera_generation_ = generation;
    auto& slot = groups_[group_next_];
    if (slot.live && !slot.done) Resolve(slot);
    slot = {}; slot.live = true;
    auto& r = slot.record;
    r.camera_generation = generation; r.group_id = set.group_id; r.group_timestamp_ns = set.group_timestamp_ns;
    r.group_raw_ns = set.reserved[SC132_GROUP_RAW_LO] | (uint64_t(set.reserved[SC132_GROUP_RAW_HI]) << 32);
    r.group_uncertainty_ns = set.reserved[SC132_GROUP_UNCERTAINTY_NS];
    for (unsigned i = 0; i < set.camera_count; ++i) {
      const auto& item = set.items[i];
      if (item.camera_id >= 4) throw std::runtime_error("invalid FSYNC camera ID");
      r.camera_mask |= 1U << item.camera_id; r.frame_ids[item.camera_id] = item.frame_id;
      r.camera_timestamps_ns[item.camera_id] = item.timestamp_ns;
    }
    if (r.group_raw_ns <= group_watermark_) throw std::runtime_error("nonmonotonic FSYNC group RAW");
    group_watermark_ = r.group_raw_ns;
    group_next_ = (group_next_ + 1) % groups_.size(); Drain(false);
  }
  void ObserveEvent(const icm42670_fsync_event_t& event) {
    std::lock_guard<RealtimeMutex> lock(mutex_);
    if (imu_generation_ && imu_generation_ != event.session_generation) throw std::runtime_error("IMU restarted during FSYNC association");
    imu_generation_ = event.session_generation;
    if (event.fsync_raw_timestamp_ns <= event_watermark_) throw std::runtime_error("nonmonotonic FSYNC edge RAW");
    auto& slot = events_[event_next_];
    if (slot.live && !slot.reported) ReportEvent(slot);
    slot = {}; slot.live = true; slot.event = event;
    event_watermark_ = event.fsync_raw_timestamp_ns;
    event_next_ = (event_next_ + 1) % events_.size(); Drain(false);
  }
  void Finish() { std::lock_guard<RealtimeMutex> lock(mutex_); Drain(true); }
 private:
  struct Group { FsyncAssociationRecord record{}; bool live = false, done = false; };
  struct Event { icm42670_fsync_event_t event{}; bool live = false, reported = false; };
  static bool Overlap(const FsyncAssociationRecord& r, const icm42670_fsync_event_t& e) {
    const uint64_t a = r.group_raw_ns, b = e.fsync_raw_timestamp_ns;
    return (a >= b ? a-b : b-a) <= r.group_uncertainty_ns + e.uncertainty_ns;
  }
  void Resolve(Group& group) {
    auto& r = group.record;
    unsigned candidates = 0; Event* selected = nullptr;
    for (auto& e : events_) if (e.live && Overlap(r,e.event)) { ++candidates; selected = &e; }
    r.status = candidates ? 3U : 2U;
    if (candidates == 1) {
      unsigned owners = 0;
      for (const auto& g : groups_) if (g.live && Overlap(g.record,selected->event)) ++owners;
      if (owners == 1 && !selected->reported) { r.status = 1; r.event = selected->event; selected->reported = true; }
    }
    group.done = true; if (sink_) sink_(r,user_);
  }
  void ReportEvent(Event& event) {
    FsyncAssociationRecord record{}; record.status = 2; record.event = event.event;
    for (const auto& g : groups_) if (g.live && Overlap(g.record,event.event)) { record.status = 3; break; }
    event.reported = true; if (sink_) sink_(record,user_);
  }
  void Drain(bool finish) {
    // Two later hardware edges/groups close the candidate interval; arrival order
    // and realtime steps do not affect matching. History also detects ambiguity.
    for (auto& g : groups_) if (g.live && !g.done && (finish ||
        (event_watermark_ > g.record.group_raw_ns + g.record.group_uncertainty_ns + 200000ULL &&
         group_watermark_ > g.record.group_raw_ns + g.record.group_uncertainty_ns + 200000ULL))) Resolve(g);
    if (finish) for (auto& e : events_) if (e.live && !e.reported) ReportEvent(e);
  }
  Sink sink_; void* user_;
  RealtimeMutex mutex_;
  std::array<Group,128> groups_{}; std::array<Event,256> events_{};
  size_t group_next_ = 0, event_next_ = 0;
  uint32_t camera_generation_ = 0, imu_generation_ = 0;
  uint64_t group_watermark_ = 0, event_watermark_ = 0;
};
}
