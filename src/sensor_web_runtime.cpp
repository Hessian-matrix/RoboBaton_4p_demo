#include "sensor_web_runtime.h"
#include "sensor_web_protocol.h"
#include "sensor_web_imu_filter.h"
#include "cam_demo_media.h"
#include "sensor_recording.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace robobaton_demo {
namespace {
using namespace sensor_web;
constexpr uint32_t kSlots = 4;
constexpr uint32_t kQueuedBytes = 4U * 1024U * 1024U;
void CloseFd(int& fd) noexcept { if (fd >= 0) ::close(fd); fd = -1; }
bool Pair(int (&fds)[2]) noexcept {
  return socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0;
}
std::string ExecutableDirectory() {
  std::array<char, 4096> path{};
  const ssize_t n = readlink("/proc/self/exe", path.data(), path.size()-1);
  if (n <= 0) return {};
  std::string name(path.data(), static_cast<size_t>(n));
  return name.substr(0, name.find_last_of('/'));
}
}

class SensorWebRuntime::Impl {
 public:
  struct Slot {
    std::unique_ptr<uint8_t[]> data;
    uint32_t size = 0;
    uint32_t codec = 0;
    uint32_t flags = 0;
    uint64_t sequence = 0;
    uint64_t source_ns = 0;
    uint64_t media_ns = 0;
    uint64_t stream_generation = 0;
  };
  struct Queue {
    std::array<Slot, kSlots> slots;
    std::atomic<uint64_t> head{0}, tail{0};
    std::atomic<uint32_t> bytes{0};
    std::atomic<uint64_t> drops{0};
    uint64_t sequence = 0;
    uint32_t next_chunk = 0;
  };
  std::array<Queue, kMaxChannels> queues;
  Options options;
  const MediaChannels* media = nullptr;
  SensorRecording* recording = nullptr;
  uint64_t generation = 0;
  std::atomic<bool> accepting{false}, stopping{false};
  std::atomic<uint32_t> demand{0};
  std::atomic<uint64_t> control_ns{0}, imu_drops{0};
  std::atomic<uint32_t> status_bits{0};
  uint64_t last_imu_ns = 0;
  PreviewImuFilter imu_filter;
  int video[2]{-1,-1}, imu[2]{-1,-1}, control[2]{-1,-1}, life[2]{-1,-1};
  pid_t child = -1;
  std::thread sender;
  std::string directory;

  bool Start(const Options& input, const MediaChannels* channels) {
    options = input;
    imu_filter = PreviewImuFilter(input.imu_sample_rate_hz, input.web_imu_push_hz);
    media = channels;
    if (!input.web_enabled) return true;
    if (getrandom(&generation, sizeof(generation), 0) != sizeof(generation) || generation == 0) return false;
    char pattern[] = "/tmp/sensor-web-XXXXXX";
    const char* created = mkdtemp(pattern);
    if (!created) return false;
    directory = created;
    if (!Pair(video) || !Pair(imu) || !Pair(control) || pipe2(life, O_CLOEXEC) != 0) return false;
    int capacity = 4 * 1024 * 1024;
    setsockopt(video[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
    setsockopt(video[1], SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity));
    for (int id = 0; id < kMaxChannels; ++id) {
      if (CameraMaskContains(input.camera_mask, id)) {
        for (auto& slot : queues[id].slots) slot.data.reset(new uint8_t[kMaxAuBytes]);
      }
    }
    const std::string config_path = directory + "/config.json";
    int config = open(config_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (config < 0) return false;
    char text[1536];
    const int size = snprintf(text, sizeof(text),
      "{\"schema_version\":1,\"generation\":\"%llu\",\"camera_mask\":%u,"
      "\"width\":%d,\"height\":%d,\"fps\":%d,\"bitrate_bps\":%llu,\"codec\":\"%s\",\"rtsp_enabled\":%s,"
      "\"web_enabled\":true,\"listen_address\":\"%s\",\"port\":%u,\"ice_port\":8189,"
      "\"source_rate_hz\":%u,\"telemetry_limit_hz\":%u,\"max_video_sessions\":%u,\"max_imu_clients\":%u}",
      static_cast<unsigned long long>(generation), input.camera_mask,
      OutputWidth(input), OutputHeight(input), input.fps,
      static_cast<unsigned long long>(input.bps) * 1000ULL, VideoCodecName(input.video_codec),
      input.rtsp_enabled ? "true" : "false", input.web_listen_address.c_str(), input.web_port,
      input.imu_sample_rate_hz, input.web_imu_push_hz, input.web_max_video_sessions, input.web_max_imu_clients);
    if (size <= 0 || size >= static_cast<int>(sizeof(text)) ||
        write(config, text, static_cast<size_t>(size)) != size || lseek(config, 0, SEEK_SET) < 0) {
      CloseFd(config); return false;
    }
    // Stable inherited descriptors avoid path access after the Web process drops privileges.
    const int sources[]{config, video[1], imu[1], control[1], life[0]};
    std::array<int, 5> copies{};
    copies.fill(-1);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) { CloseFd(config); return false; }
    bool valid = true;
    for (size_t i = 0; i < copies.size(); ++i) {
      copies[i] = fcntl(sources[i], F_DUPFD_CLOEXEC, 32);
      if (copies[i] < 0 || posix_spawn_file_actions_adddup2(&actions, copies[i], static_cast<int>(3+i)) != 0) valid = false;
    }
    // closefrom is ordered after the explicit dup2 actions; only 0..7 reach the child.
    if (posix_spawn_file_actions_addclosefrom_np(&actions, 8) != 0) valid = false;
    const std::string executable = ExecutableDirectory() + "/sensor_web";
    char* argv[]{const_cast<char*>(executable.c_str()), const_cast<char*>("--inherited-fds"), nullptr};
    const int result = valid ? posix_spawn(&child, executable.c_str(), &actions, nullptr, argv, environ) : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    for (auto& fd : copies) CloseFd(fd);
    CloseFd(config); CloseFd(video[1]); CloseFd(imu[1]); CloseFd(control[1]); CloseFd(life[0]);
    if (result != 0) { child = -1; errno = result; return false; }
    pollfd ready{control[0], POLLIN, 0};
    uint8_t message[32]{};
    if (poll(&ready, 1, 3000) <= 0 || recv(control[0], message, sizeof(message), 0) != 32 ||
        Get32(message) != kControlMagic || Get64(message+8) != generation) return false;
    control_ns.store(SteadyClockNowNs());
    accepting.store(true, std::memory_order_release);
    sender = std::thread([this] { RunSender(); });
    return true;
  }

  void UpdateControl() noexcept {
    uint8_t message[65];
    while (true) {
      const ssize_t n = recv(control[0], message, sizeof(message), MSG_DONTWAIT);
      if (n < 0) break;
      if (n == 32 && Get32(message) == kControlMagic && message[4] == 0 && message[5] == 1 &&
          message[6] == 0 && message[7] == 32 && Get64(message+8) == generation) {
        demand.store(Get32(message+24) & 0x1fU, std::memory_order_release);
        control_ns.store(SteadyClockNowNs(), std::memory_order_release);
      }
      else if (recording && n == 64 && Get32(message) == kRecordingCommandMagic &&
               message[4] == 0 && message[5] == 1 && message[6] == 0 && message[7] == 64 &&
               Get64(message+8) == generation && Get32(message+36) == 0 && Get64(message+56) == 0) {
        RecordingCommand command;
        command.action = Get32(message+16); command.format = Get32(message+20);
        command.storage = Get32(message+24); command.frame_skip = Get32(message+28);
        command.drop_min_interval = Get32(message+32); command.request_id = Get64(message+40);
        command.expected_session_id = Get64(message+48);
        try { recording->Submit(command); } catch (...) {}
      }
    }
    if (SteadyClockNowNs() - control_ns.load(std::memory_order_acquire) > 1000000000ULL) demand.store(0);
  }

  void RunSender() noexcept {
    uint64_t previous_status = 0;
    while (!stopping.load(std::memory_order_acquire)) {
      UpdateControl();
      const uint64_t now = SteadyClockNowNs();
      bool blocked = false;
      for (int id = 0; id < kMaxChannels; ++id) {
        auto& q = queues[id];
        const uint64_t tail = q.tail.load(std::memory_order_relaxed);
        if (tail == q.head.load(std::memory_order_acquire)) continue;
        auto& slot = q.slots[tail % kSlots];
        // Publication can occur after the outer status clock was sampled.
        const uint64_t slot_now = SteadyClockNowNs();
        bool ok = accepting.load(std::memory_order_acquire) &&
          (demand.load() & (1U << id)) && slot.stream_generation == media->StreamGeneration(id) &&
          slot_now >= slot.media_ns && slot_now - slot.media_ns <= kMaxAgeNs;
        const uint32_t chunks = (slot.size + kChunkBytes - 1U) / kChunkBytes;
        uint32_t& chunk = q.next_chunk;
        for (; ok && chunk < chunks; ++chunk) {
          if (SteadyClockNowNs() - slot.media_ns > kMaxAgeNs ||
              !accepting.load(std::memory_order_acquire)) { ok = false; break; }
          const uint32_t offset = chunk * kChunkBytes;
          const uint32_t bytes = std::min<uint32_t>(kChunkBytes, slot.size-offset);
          std::array<uint8_t, kVideoHeader> header{};
          Header(header.data(), kVideoMagic, kVideoHeader, generation);
          Put64(header.data()+16, slot.stream_generation);
          Put32(header.data()+24, static_cast<uint32_t>(id));
          Put32(header.data()+28, slot.codec); Put32(header.data()+32, slot.flags);
          Put32(header.data()+36, slot.size); Put64(header.data()+40, slot.sequence);
          Put64(header.data()+48, slot.source_ns); Put64(header.data()+56, slot.media_ns);
          Put32(header.data()+64, offset); Put16(header.data()+68, chunk);
          Put16(header.data()+70, chunks); Put32(header.data()+72, bytes);
          iovec vectors[]{{header.data(), header.size()}, {slot.data.get()+offset, bytes}};
          msghdr message{}; message.msg_iov = vectors; message.msg_iovlen = 2;
          const ssize_t sent = sendmsg(video[0], &message, MSG_DONTWAIT | MSG_NOSIGNAL);
          if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            blocked = true;
            break;
          }
          ok = sent == static_cast<ssize_t>(header.size()+bytes);
        }
        // Keep the owned slot and exact chunk offset until the receiver drains.
        // The next turn rechecks demand, generation and the original AU deadline.
        if (ok && chunk < chunks) continue;
        q.next_chunk = 0;
        if (!ok) q.drops.fetch_add(1, std::memory_order_relaxed);
        q.bytes.fetch_sub(slot.size, std::memory_order_release);
        q.tail.store(tail+1, std::memory_order_release);
      }
      if (now-previous_status >= 100000000ULL) {
        std::array<uint8_t, 64> status{};
        Header(status.data(), kStatusMagic, status.size(), generation);
        Put32(status.data()+16, status_bits.load());
        Put64(status.data()+24, imu_drops.load());
        for (size_t id=0; id<queues.size(); ++id) Put64(status.data()+32+8*id, queues[id].drops.load());
        (void)send(control[0], status.data(), status.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        previous_status = now;
        if (recording) {
          try {
            const std::string json = recording->StatusJson();
            if (json.size() <= 4080) {
              std::array<uint8_t, 16> header{};
              Header(header.data(), kRecordingStatusMagic, static_cast<uint16_t>(16+json.size()), generation);
              iovec parts[]{{header.data(), header.size()}, {const_cast<char*>(json.data()), json.size()}};
              msghdr packet{}; packet.msg_iov = parts; packet.msg_iovlen = 2;
              (void)sendmsg(control[0], &packet, MSG_DONTWAIT | MSG_NOSIGNAL);
            }
          } catch (...) {}
        }
      }
      pollfd pending[]{{control[0], POLLIN, 0},
                       {video[0], static_cast<short>(blocked ? POLLOUT : 0), 0}};
      (void)poll(pending, 2, 2);
    }
    demand.store(0);
  }

  void Encoded(int id, const prrtsp_encoded_frame_v2& frame) noexcept {
    if (id < 0 || id >= kMaxChannels || !accepting.load(std::memory_order_acquire) ||
        !(demand.load(std::memory_order_acquire) & (1U << id))) return;
    auto& q = queues[id];
    const uint64_t sequence = ++q.sequence;
    const uint64_t head = q.head.load(std::memory_order_relaxed);
    if (frame.struct_size != PRRTSP_ENCODED_FRAME_V2_0_SIZE || !frame.data_address ||
        !frame.size_bytes || frame.size_bytes > kMaxAuBytes ||
        head-q.tail.load(std::memory_order_acquire) >= kSlots ||
        q.bytes.load(std::memory_order_acquire)+frame.size_bytes > kQueuedBytes) {
      q.drops.fetch_add(1, std::memory_order_relaxed); return;
    }
    auto& slot = q.slots[head % kSlots];
    if (!slot.data) { q.drops.fetch_add(1); return; }
    slot.size = static_cast<uint32_t>(frame.size_bytes); slot.sequence = sequence;
    slot.codec = frame.codec; slot.flags = frame.flags; slot.source_ns = frame.timestamp_ns;
    slot.media_ns = SteadyClockNowNs(); slot.stream_generation = media->StreamGeneration(id);
    std::memcpy(slot.data.get(), reinterpret_cast<const void*>(static_cast<uintptr_t>(frame.data_address)), slot.size);
    q.bytes.fetch_add(slot.size, std::memory_order_release);
    q.head.store(head+1, std::memory_order_release);
  }

  void Imu(const icm42670_sample_t& sample, uint64_t now) noexcept {
    if (!accepting.load(std::memory_order_acquire) || sample.struct_size != sizeof(sample)) return;
    // Observe every native sample, including while no browser subscribes. Only
    // serialization is decimated; raw telemetry and saved samples stay intact.
    (void)imu_filter.Observe(sample);
    if (!(demand.load() & 0x10U) || now-last_imu_ns < 1000000000ULL/options.web_imu_push_hz) return;
    last_imu_ns = now;
    std::array<uint8_t, 176> message{};
    Header(message.data(), kImuMagic, message.size(), generation);
    Put64(message.data()+16, sample.sample_sequence);
    Put64(message.data()+24, sample.sample_timestamp_ns); Put64(message.data()+32, sample.host_timestamp_ns);
    PutDouble(message.data()+40, sample.temperature_c);
    for (size_t i=0; i<3; ++i) {
      PutDouble(message.data()+48+8*i, sample.accel_mps2[i]);
      PutDouble(message.data()+72+8*i, sample.gyro_rps[i]);
    }
    for (size_t i=0; i<6; ++i) PutDouble(message.data()+128+8*i, imu_filter.Values()[i]);
    Put32(message.data()+96, sample.timestamp_uncertainty_us);
    Put32(message.data()+100, sample.gpio_event_gap_count);
    Put32(message.data()+104, sample.fifo_overflow_count);
    Put32(message.data()+108, sample.mapper_failure_count);
    Put64(message.data()+112, imu_drops.load());
    if (send(imu[0], message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL) != static_cast<ssize_t>(message.size())) imu_drops.fetch_add(1);
  }

  void BeginShutdown() noexcept {
    accepting.store(false, std::memory_order_release); demand.store(0);
    // EOF closes HTTP admission immediately, without waiting for capture/recorders.
    CloseFd(life[1]);
  }
  void Stop() noexcept {
    BeginShutdown(); stopping.store(true, std::memory_order_release);
    if (sender.joinable()) sender.join();
    CloseFd(life[1]);
    if (child > 0) {
      int status = 0; bool reaped = false;
      for (unsigned i=0; i<200; ++i) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!reaped) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
      child = -1;
    }
    for (int* pair : {video, imu, control, life}) { CloseFd(pair[0]); CloseFd(pair[1]); }
    if (!directory.empty()) { unlink((directory+"/config.json").c_str()); rmdir(directory.c_str()); directory.clear(); }
  }
};

SensorWebRuntime::SensorWebRuntime() = default;
SensorWebRuntime::~SensorWebRuntime() { Stop(); }
bool SensorWebRuntime::Start(const Options& options, const MediaChannels* media) noexcept {
  if (!options.web_enabled) return true;
  try {
    impl_ = std::make_unique<Impl>();
    impl_->recording = recording_;
    if (media != nullptr && impl_->Start(options, media)) return true;
  } catch (...) {}
  std::fprintf(stderr, "web=degraded: sensor_web startup failed; capture and saving remain enabled\n");
  Stop(); return false;
}
void SensorWebRuntime::SetRecording(SensorRecording* recording) noexcept { recording_ = recording; }
void SensorWebRuntime::BeginShutdown() noexcept { if (impl_) impl_->BeginShutdown(); }
void SensorWebRuntime::Stop() noexcept { if (impl_) { impl_->Stop(); impl_.reset(); } }
void SensorWebRuntime::ObserveEncoded(int id, const prrtsp_encoded_frame_v2& frame) noexcept { if (impl_) impl_->Encoded(id, frame); }
void SensorWebRuntime::ObserveImu(const icm42670_sample_t& sample, uint64_t arrival) noexcept { if (impl_) impl_->Imu(sample, arrival); }
void SensorWebRuntime::PublishStatus(bool capture, bool encoding, bool saving, bool failed, bool rtsp_degraded) noexcept {
  if (impl_) impl_->status_bits.store((capture?1U:0U) | (encoding?2U:0U) | (saving?4U:0U) | (failed?8U:0U) | (rtsp_degraded?16U:0U));
}
void SensorWebRuntime::EncodedObserver(int id, const prrtsp_encoded_frame_v2& frame, void* user) noexcept {
  if (user) static_cast<SensorWebRuntime*>(user)->ObserveEncoded(id, frame);
}
}  // namespace robobaton_demo
