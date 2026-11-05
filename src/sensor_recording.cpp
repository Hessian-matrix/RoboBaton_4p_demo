#include "sensor_recording.h"
#include "sensor_bag_recorder.h"
#include "h264_mp4_recorder.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>

namespace robobaton_demo {
namespace {
namespace fs = std::filesystem;
uint64_t NowMs() noexcept { return SteadyClockNowNs() / 1000000ULL; }
uint64_t NewId() {
  uint64_t id = 0;
  if (getrandom(&id, sizeof(id), 0) != sizeof(id) || id == 0)
    throw std::runtime_error("recording identity unavailable");
  return id;
}
std::string Quote(const std::string& text) {
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if (c < 32) { char code[7]; std::snprintf(code, sizeof(code), "\\u%04x", c); result += code; }
    else result += static_cast<char>(c);
  }
  return result + '"';
}
// TF must be the actual mounted filesystem, never an absent card's eMMC directory.
fs::path StorageRoot(uint32_t storage) {
  if (storage == 0) return "/root/demo/save_data";
  const fs::path mount = "/mnt/sdcard";
  struct stat device{}, parent{};
  if (lstat(mount.c_str(), &device) != 0 || !S_ISDIR(device.st_mode) ||
      stat("/mnt", &parent) != 0 || device.st_dev == parent.st_dev)
    throw std::runtime_error("TF card is not mounted at /mnt/sdcard");
  return mount / "recordings";
}
}

class SensorRecording::Impl {
 public:
  explicit Impl(const Options& input) : options(input) { worker = std::thread([this] { Run(); }); }
  ~Impl() { Shutdown(false, nullptr); }
  Options options;
  mutable std::mutex state_mutex;
  std::condition_variable wake;
  std::thread worker;
  bool closing = false, pending = false, ready = false, source_success = false;
  bool busy = false;
  RecordingCommand command;
  uint64_t id = NewId(), request_id = 0, started_ms = 0, ended_ms = 0;
  int reply = 200;
  std::string state = "idle", format, storage = "runtime", path, error, outcome;
  bool complete = false, cleanup = true;
  uint64_t bytes = 0, dropped = 0, free_bytes = 0;
  uint64_t dropped_frames = 0;
  std::atomic<bool> saving{false};
  RealtimeMutex admission;
  SensorBagRecorder* bag = nullptr;
  H264Mp4Recorder* mp4 = nullptr;
  std::unique_ptr<SensorBagRecorder> owned_bag;
  std::unique_ptr<H264Mp4Recorder> owned_mp4;
  H264Mp4SourceHealth health;
  icm42670_sample_t last{};
  bool have_sample = false, admission_failed = false, adopted = false;
  bool stop_at_group = false, group_closed = false;
  uint64_t encoded_timestamp = 0;
  uint32_t encoded_mask = 0;
  std::condition_variable_any encoded_progress;
  icm42670_runtime_health_t final_health = ICM42670_RUNTIME_HEALTH_INIT;

  void PrepareRecording() {
    health = {};
    have_sample = false; admission_failed = false;
    stop_at_group = group_closed = false;
    encoded_timestamp = 0; encoded_mask = 0;
  }
  void Adopt(SensorBagRecorder* input_bag, H264Mp4Recorder* input_mp4, const std::string& input_path) {
    const uint64_t next_id = NewId();
    std::lock_guard<RealtimeMutex> gate(admission);
    PrepareRecording();
    bag = input_bag; mp4 = input_mp4; adopted = true;
    std::lock_guard<std::mutex> lock(state_mutex);
    id = next_id; path = input_path; format = mp4 ? "mp4" : "rosbag";
    state = "recording"; started_ms = NowMs(); saving.store(true);
  }
  void Submit(const RecordingCommand& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    request_id = input.request_id;
    reply = 400;
    if (!input.request_id || (input.action != 1 && input.action != 2)) return;
    if (closing || !ready) { reply = 503; return; }
    if (input.expected_session_id != id || busy || pending) { reply = 409; return; }
    if (input.action == 1) {
      if (state == "recording" || state == "stopping" || !cleanup) { reply = 409; return; }
      if ((input.format != 1 && input.format != 2) || input.storage > 1 ||
          input.frame_skip > 1 || input.drop_min_interval < 2 ||
          (input.frame_skip && input.drop_min_interval != 2) ||
          (input.format == 2 && (!Mp4Available() || input.frame_skip))) return;
      state = "starting";
      path.clear(); error.clear(); outcome.clear(); complete = false; cleanup = true;
      format = input.format == 1 ? "rosbag" : "mp4";
      storage = input.storage ? "tf" : "runtime";
      bytes = dropped = dropped_frames = free_bytes = 0;
      started_ms = ended_ms = 0;
    } else {
      if (state != "recording") { reply = 409; return; }
      state = "stopping";
    }
    reply = 202; busy = true; command = input; pending = true; wake.notify_one();
  }
  bool Mp4Available() const noexcept {
    return options.video_codec == VideoCodec::kH264 && options.camera_mask == 15 &&
           (options.web_enabled || options.rtsp_enabled || !options.record_mp4_directory.empty());
  }
  std::string Json() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto duration = started_ms ? (ended_ms ? ended_ms : NowMs()) - started_ms : 0;
    std::ostringstream out;
    out << "{\"request_id\":\"" << request_id << "\",\"reply_code\":" << reply
        << ",\"session_id\":\"" << id << "\",\"state\":" << Quote(state)
        << ",\"format\":" << Quote(format) << ",\"storage\":" << Quote(storage)
        << ",\"path\":" << Quote(path.substr(0, 2048)) << ",\"error\":" << Quote(error.substr(0, 768))
        << ",\"outcome\":" << Quote(outcome) << ",\"data_complete\":" << (complete ? "true" : "false")
        << ",\"cleanup_complete\":" << (cleanup ? "true" : "false")
        << ",\"elapsed_ms\":\"" << duration << "\",\"bytes_written\":\"" << bytes
        << "\",\"dropped_groups\":\"" << dropped << "\",\"dropped_frames\":\"" << dropped_frames << "\",\"free_bytes\":\"" << free_bytes
        << "\",\"can_start\":" << (ready && !closing && !busy && cleanup && state != "recording" ? "true" : "false")
        << ",\"mp4_available\":" << (Mp4Available() ? "true" : "false")
        << ",\"health_scope\":\"callback_window\"}";
    return out.str();
  }
  void Start(const RecordingCommand& input) {
    const auto next_id = NewId();
    const auto root = StorageRoot(input.storage);
    if (fs::is_symlink(root)) throw std::runtime_error("recording root must not be a symlink");
    fs::create_directories(root);
    struct statvfs space{};
    if (statvfs(root.c_str(), &space) != 0) throw std::runtime_error("recording storage unavailable");
    if (space.f_bavail * static_cast<uint64_t>(space.f_frsize) < 64ULL*1024*1024)
      throw std::runtime_error("recording storage has less than 64 MiB free");
    const auto output = root / ("session-" + std::to_string(next_id) + (input.format == 1 ? ".bag" : ""));
    Options selected = options;
    selected.record_frame_skip = input.frame_skip;
    selected.record_drop_min_interval = input.drop_min_interval;
    selected.record_bag_path.clear(); selected.record_mp4_directory.clear();
    std::unique_ptr<SensorBagRecorder> next_bag;
    std::unique_ptr<H264Mp4Recorder> next_mp4;
    if (input.format == 1) {
      selected.record_bag_path = output.string();
      next_bag = std::make_unique<SensorBagRecorder>(); next_bag->Start(selected, output.string());
    } else {
      selected.record_mp4_directory = output.string();
      next_mp4 = std::make_unique<H264Mp4Recorder>(); next_mp4->Start(selected, output.string());
    }
    {
      std::lock_guard<RealtimeMutex> gate(admission);
      PrepareRecording();
      owned_bag = std::move(next_bag); owned_mp4 = std::move(next_mp4);
      bag = owned_bag.get(); mp4 = owned_mp4.get(); adopted = false;
      saving.store(true, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    id = next_id; path = output.string(); format = input.format == 1 ? "rosbag" : "mp4";
    storage = input.storage ? "tf" : "runtime"; state = "recording";
    error.clear(); outcome.clear(); complete = false; cleanup = true;
    bytes = dropped = dropped_frames = 0; started_ms = NowMs(); ended_ms = 0;
  }
  void ObserveImu(const OutputImuSample& mapped) noexcept {
    std::lock_guard<RealtimeMutex> gate(admission);
    if (!saving.load(std::memory_order_acquire)) return;
    const auto& sample = mapped.native;
    if (sample.struct_size != sizeof(sample) || !sample.session_generation ||
        !sample.evidence_id || !sample.sample_monotonic_ns || !mapped.realtime_ns ||
        (sample.timestamp_mode != ICM42670_TIMESTAMP_MODE_HOST &&
         sample.timestamp_mode != ICM42670_TIMESTAMP_MODE_TMST) ||
        (sample.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST && !sample.sample_raw_ns)) {
      ++health.invalid_samples; return;
    }
    if (have_sample) {
      if (sample.session_generation != last.session_generation ||
          sample.timestamp_mode != last.timestamp_mode) ++health.invalid_samples;
      if (sample.sample_sequence == last.sample_sequence) ++health.sequence_duplicates;
      else if (sample.sample_sequence < last.sample_sequence) ++health.sequence_regressions;
      else health.sequence_gaps += sample.sample_sequence - last.sample_sequence - 1;
      const bool raw = sample.timestamp_mode == ICM42670_TIMESTAMP_MODE_TMST;
      const auto now = raw ? sample.sample_raw_ns : sample.sample_monotonic_ns;
      const auto previous = raw ? last.sample_raw_ns : last.sample_monotonic_ns;
      if (now == previous) ++health.timestamp_duplicates;
      else if (now < previous) ++health.timestamp_regressions;
    }
    have_sample = true; ++health.samples; last = sample;
    try { if (bag) bag->ObserveImu(mapped); if (mp4) mp4->ObserveImu(mapped); }
    catch (...) { admission_failed = true; }
  }
  void Stop(bool success) noexcept {
    SensorBagRecorder* finished_bag;
    H264Mp4Recorder* finished_mp4;
    if (!saving.load(std::memory_order_acquire)) return;
    icm42670_runtime_health_t final;
    { std::lock_guard<std::mutex> lock(state_mutex); final = final_health; state = "stopping"; busy = true; }
    {
      std::unique_lock<RealtimeMutex> gate(admission);
      if (!saving.load()) return;
      if (mp4 && success) {
        // Prefer a complete common encoded group without stalling shutdown.
        stop_at_group = true;
        encoded_progress.wait_for(gate, std::chrono::milliseconds(250), [this] { return group_closed; });
      }
      saving.store(false, std::memory_order_release);
      finished_bag = bag; finished_mp4 = mp4;
      if (finished_mp4) {
        health.callback_window_valid = !adopted && have_sample;
        if (adopted) health.producer = final;
        finished_mp4->SetSourceHealth(health);
      }
      success = success && !admission_failed && have_sample && health.invalid_samples == 0 &&
          health.sequence_gaps == 0 && health.sequence_duplicates == 0 &&
          health.sequence_regressions == 0 && health.timestamp_duplicates == 0 &&
          health.timestamp_regressions == 0;
      bag = nullptr; mp4 = nullptr;
    }
    std::string result_path, result_error, result_outcome;
    bool data_complete = false, cleanup_complete = true;
    uint64_t final_bytes = 0, final_dropped = 0, final_frames = 0;
    if (finished_bag) {
      const auto result = finished_bag->Finish(success);
      const auto stats = finished_bag->SnapshotStats();
      final_bytes = stats.writer_stats.raw_write_bytes; final_dropped = stats.recorder_dropped_groups;
      result_path = result.published_path.empty() ? result.quarantine_path : result.published_path;
      result_error = result.error; result_outcome = SensorBagFinishOutcomeName(result.outcome);
      data_complete = result.data_complete; cleanup_complete = result.cleanup_complete;
    } else if (finished_mp4) {
      const auto result = finished_mp4->Finish(success);
      const auto stats = finished_mp4->SnapshotStats();
      final_bytes = stats.encoded_bytes_written; final_frames = stats.encoded_frames_dropped;
      result_path = result.published_path; result_error = result.error;
      result_outcome = H264Mp4FinishOutcomeName(result.outcome);
      data_complete = result.data_complete; cleanup_complete = result.cleanup_complete;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    bytes = final_bytes; dropped = final_dropped; dropped_frames = final_frames;
    if (!result_path.empty()) path = result_path;
    error = result_error; outcome = result_outcome; complete = data_complete; cleanup = cleanup_complete;
    state = complete ? "complete" : (result_outcome == "published_partial" ? "partial" : "failed");
    ended_ms = NowMs();
    std::fprintf(stdout, "SENSOR_RECORDING_RESULT session_id=%llu format=%s outcome=%s data_complete=%s cleanup_complete=%s\n",
                 static_cast<unsigned long long>(id), format.c_str(), outcome.c_str(), complete ? "yes" : "no", cleanup ? "yes" : "no");
  }
  void Refresh() {
    uint64_t written = 0, lost = 0, lost_frames = 0;
    bool failed = false;
    {
      std::lock_guard<RealtimeMutex> gate(admission);
      if (bag) { const auto stats = bag->SnapshotStats(); written = stats.writer_stats.raw_write_bytes;
        lost = stats.recorder_dropped_groups; failed = bag->HasFatalError() || bag->DropPolicyStopped(); }
      if (mp4) { const auto stats = mp4->SnapshotStats(); written = stats.encoded_bytes_written;
        lost_frames = stats.encoded_frames_dropped; failed = mp4->HasFatalError(); }
      failed = saving.load() && (failed || admission_failed);
    }
    std::string current_path;
    { std::lock_guard<std::mutex> lock(state_mutex);
      if (saving.load()) { bytes = written; dropped = lost; dropped_frames = lost_frames; }
      current_path = path;
      if (failed) { busy = true; state = "stopping"; }
    }
    struct statvfs space{};
    if (!current_path.empty() && statvfs(fs::path(current_path).parent_path().c_str(), &space) == 0) {
      std::lock_guard<std::mutex> lock(state_mutex);
      free_bytes = space.f_bavail * static_cast<uint64_t>(space.f_frsize);
    }
    if (failed) { Stop(false); std::lock_guard<std::mutex> lock(state_mutex); busy = false; }
  }
  void Run() noexcept {
    for (;;) {
      RecordingCommand next;
      bool execute = false, quit = false;
      {
        std::unique_lock<std::mutex> lock(state_mutex);
        wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return pending || closing; });
        quit = closing;
        if (!quit && pending) { next = command; pending = false; execute = true; }
      }
      if (quit) { Stop(source_success); return; }
      try {
        if (execute) { if (next.action == 1) Start(next); else Stop(true); }
        Refresh();
      } catch (const std::exception& failure) {
        std::lock_guard<std::mutex> lock(state_mutex);
        state = "failed"; error = failure.what(); ended_ms = NowMs();
      } catch (...) {
        std::lock_guard<std::mutex> lock(state_mutex);
        state = "failed"; error = "recording lifecycle failure"; ended_ms = NowMs();
      }
      if (execute) { std::lock_guard<std::mutex> lock(state_mutex); busy = false; }
    }
  }
  void Shutdown(bool success, const icm42670_runtime_health_t* final) noexcept {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      closing = true; source_success = success;
      if (final) final_health = *final;
      wake.notify_one();
    }
    if (worker.joinable()) worker.join();
  }
};

SensorRecording::SensorRecording(const Options& options) : impl_(std::make_unique<Impl>(options)) {}
SensorRecording::~SensorRecording() = default;
void SensorRecording::AdoptStartup(SensorBagRecorder* bag, H264Mp4Recorder* mp4, const std::string& path) { impl_->Adopt(bag, mp4, path); }
void SensorRecording::SetSourceReady() noexcept { std::lock_guard<std::mutex> lock(impl_->state_mutex); impl_->ready = true; }
void SensorRecording::Submit(const RecordingCommand& command) { impl_->Submit(command); }
std::string SensorRecording::StatusJson() const { return impl_->Json(); }
bool SensorRecording::Saving() const noexcept { return impl_->saving.load(std::memory_order_acquire); }
void SensorRecording::ObserveImu(const OutputImuSample& sample) noexcept { impl_->ObserveImu(sample); }
void SensorRecording::ObserveFrameSet(const sc132_frame_set_t& set) noexcept {
  std::lock_guard<RealtimeMutex> gate(impl_->admission);
  if (!impl_->saving.load()) return;
  try {
    if (impl_->bag) (void)impl_->bag->TryAcceptFrameSet(set);
    if (impl_->mp4) impl_->mp4->ObserveFrameSet(set);
  } catch (...) { impl_->admission_failed = true; }
}
void SensorRecording::ObserveEncoded(int id, const prrtsp_encoded_frame_v2& frame) noexcept {
  std::lock_guard<RealtimeMutex> gate(impl_->admission);
  if (!impl_->mp4 || impl_->group_closed || id < 0 || id >= kMaxChannels) return;
  impl_->mp4->ObserveEncodedFrame(id, frame, !impl_->adopted);
  if (frame.timestamp_ns != impl_->encoded_timestamp) {
    impl_->encoded_timestamp = frame.timestamp_ns; impl_->encoded_mask = 0;
  }
  impl_->encoded_mask |= 1U << static_cast<uint32_t>(id);
  if (impl_->stop_at_group && impl_->encoded_mask == 15) {
    impl_->group_closed = true; impl_->encoded_progress.notify_all();
  }
}
void SensorRecording::Shutdown(bool success, const icm42670_runtime_health_t* health) noexcept { impl_->Shutdown(success, health); }
void SensorRecording::EncodedObserver(int id, const prrtsp_encoded_frame_v2& frame, void* user) noexcept {
  if (user) static_cast<SensorRecording*>(user)->ObserveEncoded(id, frame);
}
}  // namespace robobaton_demo
