#include "h264_mp4_recorder.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace robobaton_demo {
namespace {

namespace fs = std::filesystem;

constexpr size_t kEncodedQueueCapacity = 512U;
constexpr uint64_t kEncodedQueueByteCapacity = 128U * 1024U * 1024U;
constexpr size_t kImuQueueCapacity = 8192U;
constexpr size_t kImuWriteBatch = 256U;
constexpr uint64_t kMaxEncodedFrameBytes = 64U * 1024U * 1024U;
constexpr uint32_t kFfmpegRemuxBaseTimeoutMs = 30000U;
constexpr uint32_t kFfmpegRemuxMaxTimeoutMs = 300000U;
constexpr uint64_t kFfmpegRemuxMinimumBytesPerSecond = 5U * 1024U * 1024U;
constexpr uint32_t kFfmpegTerminateGraceMs = 2000U;
constexpr uint32_t kMaxCompleteImuTimestampUncertaintyUs = 200U;
constexpr const char* kSessionSchema = "robobaton_h264_mp4_session_v1";
constexpr const char* kReceiptSchema =
    "robobaton_h264_mp4_publication_receipt_v1";
constexpr const char* kPublicationIncompleteMarker = ".publication_incomplete";

bool RenameNoReplace(const fs::path& source, const fs::path& destination,
                     std::error_code* error) noexcept {
  const long result = ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(),
                                AT_FDCWD, destination.c_str(), RENAME_NOREPLACE);
  if (result == 0) {
    if (error != nullptr) error->clear();
    return true;
  }
  if (error != nullptr) {
    *error = std::error_code(errno, std::generic_category());
  }
  return false;
}

std::string ErrnoText(const char* operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

void AppendFinishError(std::string* target,
                       const std::string& message) noexcept {
  if (target == nullptr || message.empty()) return;
  try {
    if (!target->empty()) target->append("; ");
    target->append(message);
  } catch (...) {
  }
}

std::string JsonEscape(const std::string& text) {
  std::ostringstream output;
  for (unsigned char value : text) {
    switch (value) {
      case '\\': output << "\\\\"; break;
      case '"': output << "\\\""; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (value < 0x20U) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned>(value) << std::dec;
        } else {
          output << static_cast<char>(value);
        }
    }
  }
  return output.str();
}

void WriteCameraCounterObject(
    std::ostream& output,
    const std::array<uint64_t, kMaxChannels>& counters) {
  output << "{";
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (camera_id != 0) output << ", ";
    output << "\"camera" << camera_id << "\": " << counters[camera_id];
  }
  output << "}";
}

bool CameraEnabled(uint32_t mask, int camera_id) {
  return camera_id >= 0 && camera_id < kMaxChannels &&
         (mask & (1U << static_cast<uint32_t>(camera_id))) != 0U;
}

struct H264AccessUnitInfo {
  bool has_vcl = false;
  bool has_idr = false;
  bool has_sps = false;
  bool has_pps = false;
  size_t sps_begin = 0U;
  size_t sps_end = 0U;
  size_t pps_begin = 0U;
  size_t pps_end = 0U;
};

bool FindAnnexBStartCode(const uint8_t* data, size_t size, size_t from,
                         size_t* start, size_t* nal_header) noexcept {
  if (data == nullptr || start == nullptr || nal_header == nullptr) return false;
  for (size_t index = from; index + 3U <= size; ++index) {
    if (data[index] != 0U || data[index + 1U] != 0U) continue;
    if (data[index + 2U] == 1U) {
      *start = index;
      *nal_header = index + 3U;
      return *nal_header < size;
    }
    if (index + 3U < size && data[index + 2U] == 0U &&
        data[index + 3U] == 1U) {
      *start = index;
      *nal_header = index + 4U;
      return *nal_header < size;
    }
  }
  return false;
}

bool ParseH264AccessUnit(const uint8_t* data, size_t size,
                         H264AccessUnitInfo* info) noexcept {
  if (info == nullptr) return false;
  H264AccessUnitInfo parsed;
  size_t current_start = 0U;
  size_t current_header = 0U;
  if (!FindAnnexBStartCode(data, size, 0U, &current_start, &current_header)) {
    return false;
  }

  // 每个范围保留原始 start code，缓存后可直接拼回 Annex-B 字节流。
  while (current_header < size) {
    size_t next_start = size;
    size_t next_header = size;
    const bool has_next = FindAnnexBStartCode(
        data, size, current_header + 1U, &next_start, &next_header);
    const size_t current_end = has_next ? next_start : size;
    if (current_header >= current_end) return false;

    // VCL NAL 为 type 1..5；关键帧语义只由 IDR(type 5)决定，不信任外部 flag。
    const uint8_t nal_type = data[current_header] & 0x1fU;
    if (nal_type >= 1U && nal_type <= 5U) parsed.has_vcl = true;
    if (nal_type == 5U) parsed.has_idr = true;
    if (nal_type == 7U) {
      parsed.has_sps = true;
      parsed.sps_begin = current_start;
      parsed.sps_end = current_end;
    } else if (nal_type == 8U) {
      parsed.has_pps = true;
      parsed.pps_begin = current_start;
      parsed.pps_end = current_end;
    }
    if (!has_next) break;
    current_start = next_start;
    current_header = next_header;
  }
  *info = parsed;
  return true;
}

std::string FindExecutable(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return std::string();
  }
  std::istringstream entries(path);
  std::string directory;
  while (std::getline(entries, directory, ':')) {
    if (directory.empty()) {
      directory = ".";
    }
    const fs::path candidate = fs::path(directory) / name;
    if (::access(candidate.c_str(), X_OK) == 0) {
      return fs::absolute(candidate).string();
    }
  }
  return std::string();
}

std::string GeneratePublicationId() {
  std::array<uint8_t, 16U> bytes{};
  bool complete = false;
  const int random_fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (random_fd >= 0) {
    size_t offset = 0U;
    while (offset < bytes.size()) {
      const ssize_t count = ::read(random_fd, bytes.data() + offset,
                                   bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    complete = offset == bytes.size();
    (void)::close(random_fd);
  }
  if (!complete) {
    static std::atomic<uint64_t> fallback_counter{0U};
    uint64_t first = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    first ^= static_cast<uint64_t>(::getpid()) << 32U;
    uint64_t second = fallback_counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
    second ^= first * UINT64_C(0x9e3779b97f4a7c15);
    std::memcpy(bytes.data(), &first, sizeof(first));
    std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (uint8_t byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

fs::path TrimTrailingSeparators(fs::path path) {
  const fs::path root = path.root_path();
  while (!path.empty() && path != root && path.filename().empty()) {
    path = path.parent_path();
  }
  return path;
}

bool HasReservedPartialSuffix(const fs::path& path) {
  const std::string filename = TrimTrailingSeparators(path).filename().string();
  return filename.size() >= 8U &&
         filename.compare(filename.size() - 8U, 8U, ".partial") == 0;
}

std::string CurrentUtcSessionTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  if (now != static_cast<std::time_t>(-1) && ::gmtime_r(&now, &utc) != nullptr) {
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc) > 0U) {
      return buffer;
    }
  }
  return std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
}

bool H264Mp4SessionPathExists(const fs::path& output_path) {
  return fs::exists(output_path) ||
         fs::exists(fs::path(output_path.string() + ".partial"));
}

fs::path ResolveH264Mp4OutputPath(const fs::path& requested_path) {
  const fs::path normalized_requested_path = TrimTrailingSeparators(requested_path);
  if (normalized_requested_path.filename().empty()) {
    throw std::invalid_argument(
        "H264 MP4 output directory must include a final path component");
  }
  if (!H264Mp4SessionPathExists(normalized_requested_path)) {
    return normalized_requested_path;
  }
  // 已存在的配置路径视为历史 session；新录制自动切到同级时间戳目录，避免覆盖数据。
  const std::string base_name = normalized_requested_path.filename().string();
  const fs::path parent = normalized_requested_path.parent_path();
  const std::string timestamp = CurrentUtcSessionTimestamp();
  for (uint32_t suffix = 0U; suffix < 10000U; ++suffix) {
    std::ostringstream candidate_name;
    candidate_name << base_name << '-' << timestamp;
    if (suffix != 0U) {
      candidate_name << '-' << std::setw(4) << std::setfill('0') << suffix;
    }
    const fs::path candidate = parent / candidate_name.str();
    if (!H264Mp4SessionPathExists(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("no available timestamped H264 MP4 output directory");
}

uint32_t RemuxTimeoutForBytes(uint64_t raw_bytes) noexcept {
  const uint64_t seconds = raw_bytes / kFfmpegRemuxMinimumBytesPerSecond +
      (raw_bytes % kFfmpegRemuxMinimumBytesPerSecond != 0U ? 1U : 0U);
  const uint64_t maximum_seconds =
      (kFfmpegRemuxMaxTimeoutMs - kFfmpegRemuxBaseTimeoutMs) / 1000U;
  const uint64_t bounded_seconds = std::min(seconds, maximum_seconds);
  return kFfmpegRemuxBaseTimeoutMs +
         static_cast<uint32_t>(bounded_seconds * 1000U);
}

bool FsyncFile(const fs::path& path, std::string* error) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (error != nullptr) *error = ErrnoText("open for fsync");
    return false;
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  (void)::close(fd);
  if (result != 0) {
    errno = saved_errno;
    if (error != nullptr) *error = ErrnoText("fsync file");
    return false;
  }
  return true;
}

bool FsyncDirectory(const fs::path& path, std::string* error) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    if (error != nullptr) *error = ErrnoText("open directory for fsync");
    return false;
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  (void)::close(fd);
  if (result != 0) {
    errno = saved_errno;
    if (error != nullptr) *error = ErrnoText("fsync directory");
    return false;
  }
  return true;
}

void WriteImuHeader(std::ostream& output) {
  output << "timestamp_ns,sequence,frame_id,orientation_x,orientation_y,orientation_z,"
            "orientation_w";
  for (int index = 0; index < 9; ++index) output << ",orientation_covariance_" << index;
  output << ",angular_velocity_x,angular_velocity_y,angular_velocity_z";
  for (int index = 0; index < 9; ++index) {
    output << ",angular_velocity_covariance_" << index;
  }
  output << ",linear_acceleration_x,linear_acceleration_y,linear_acceleration_z";
  for (int index = 0; index < 9; ++index) {
    output << ",linear_acceleration_covariance_" << index;
  }
  output << ",timestamp_uncertainty_us,gpio_event_gap_count,fifo_overflow_count,"
            "mapper_failure_count";
  output << '\n';
}

void WriteImuRow(std::ostream& output, const icm42688_sample_t& sample) {
  output << sample.sample_timestamp_ns << ',' << sample.sample_sequence
         << ",imu_link,0.000000000000000,0.000000000000000,0.000000000000000,"
            "1.000000000000000";
  output << ",-1.000000000000000";
  for (int index = 1; index < 9; ++index) output << ",0.000000000000000";
  output << ',' << std::fixed << std::setprecision(15)
         << sample.gyro_rps[0] << ',' << sample.gyro_rps[1] << ','
         << sample.gyro_rps[2];
  for (int index = 0; index < 9; ++index) output << ",0.000000000000000";
  output << ',' << sample.accel_mps2[0] << ',' << sample.accel_mps2[1] << ','
         << sample.accel_mps2[2];
  for (int index = 0; index < 9; ++index) output << ",0.000000000000000";
  output << ',' << sample.timestamp_uncertainty_us << ','
         << sample.gpio_event_gap_count << ',' << sample.fifo_overflow_count << ','
         << sample.mapper_failure_count;
  output << '\n';
}

bool FlushAndClose(std::ofstream* output) noexcept {
  if (output == nullptr || !output->is_open()) return false;
  output->flush();
  bool success = static_cast<bool>(*output);
  output->close();
  return static_cast<bool>(*output) && success;
}

void WriteCameraParameters(const fs::path& path, uint32_t camera_mask,
                           int width, int height) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("open camera_params.yaml failed: " + path.string());
  }
  output << "schema: robobaton_camera_parameters_v1\n"
         << "cameras:\n";
  for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
    if (!CameraEnabled(camera_mask, camera_id)) continue;
    output << "  camera" << camera_id << ":\n"
           << "    topic: /camera" << camera_id << "/camera_info\n"
           << "    frame_id: camera" << camera_id << "\n"
           << "    width: " << width << "\n"
           << "    height: " << height << "\n"
           << "    distortion_model: \"\"\n"
           << "    D: []\n"
           << "    K: [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n"
           << "    R: [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]\n"
           << "    P: [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, "
              "0.0, 0.0]\n"
           << "    binning_x: 0\n"
           << "    binning_y: 0\n"
           << "    roi:\n"
           << "      x_offset: 0\n"
           << "      y_offset: 0\n"
           << "      height: 0\n"
           << "      width: 0\n"
           << "      do_rectify: false\n";
  }
  if (!FlushAndClose(&output)) {
    throw std::runtime_error("write camera_params.yaml failed: " + path.string());
  }
}

void SignalProcessGroup(pid_t pid, int signal_number) noexcept {
  if (::kill(-pid, signal_number) != 0) {
    (void)::kill(pid, signal_number);
  }
}

bool ProcessGroupExists(pid_t pid) noexcept {
  if (::kill(-pid, 0) == 0) return true;
  return errno == EPERM;
}

int WaitForProcessWithDeadline(pid_t pid, const fs::path& log_path,
                               uint32_t timeout_ms,
                               uint32_t terminate_grace_ms,
                               const std::string& process_name,
                               std::string* error) {
  using Clock = std::chrono::steady_clock;
  const std::string operation =
      process_name == "ffmpeg" ? "ffmpeg remux" : process_name;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  int status = 0;
  while (true) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) break;
    if (waited < 0 && errno != EINTR) {
      if (error != nullptr) *error = ErrnoText(("waitpid " + process_name).c_str());
      return -1;
    }
    if (Clock::now() >= deadline) {
      SignalProcessGroup(pid, SIGTERM);
      bool leader_reaped = false;
      const auto term_deadline =
          Clock::now() + std::chrono::milliseconds(terminate_grace_ms);
      while (Clock::now() < term_deadline) {
        if (!leader_reaped) {
          const pid_t term_waited = ::waitpid(pid, &status, WNOHANG);
          if (term_waited == pid) {
            leader_reaped = true;
          } else if (term_waited < 0 && errno != EINTR) {
            if (error != nullptr) {
              *error = ErrnoText(("waitpid " + process_name +
                                  " after SIGTERM").c_str());
            }
            return -1;
          }
        }
        if (leader_reaped && !ProcessGroupExists(pid)) {
          if (error != nullptr) {
            *error = operation + " timed out for " + log_path.string();
          }
          return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      // Even if the group leader exited on TERM, kill any TERM-resistant descendants.
      SignalProcessGroup(pid, SIGKILL);
      if (!leader_reaped) {
        while (::waitpid(pid, &status, 0) < 0) {
          if (errno != EINTR) {
            if (error != nullptr) {
              *error = ErrnoText(("waitpid " + process_name +
                                  " after SIGKILL").c_str());
            }
            return -1;
          }
        }
      }
      if (error != nullptr) {
        *error = operation + " timed out for " + log_path.string();
      }
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error != nullptr) {
      *error = operation + " failed for " + log_path.string();
    }
    return -1;
  }
  return 0;
}

int RunProcess(const std::vector<std::string>& arguments, const fs::path& log_path,
               uint32_t timeout_ms, uint32_t terminate_grace_ms,
               bool capture_stdout, std::string* error) {
  if (arguments.empty()) {
    if (error != nullptr) *error = "empty process arguments";
    return -1;
  }
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1U);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    if (error != nullptr) *error = "posix_spawn_file_actions_init failed";
    return -1;
  }
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0) {
    (void)posix_spawn_file_actions_destroy(&actions);
    if (error != nullptr) *error = "posix_spawnattr_init failed";
    return -1;
  }
  const int group_result = posix_spawnattr_setpgroup(&attributes, 0);
  const int flags_result =
      posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (group_result != 0 || flags_result != 0) {
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (error != nullptr) *error = "prepare child process group failed";
    return -1;
  }
  int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  // 子进程不要继承调试终端的 stdin；后台进程组读终端会被停住，导致 Ctrl-C 后回收卡住。
  int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  if (log_fd < 0 || null_fd < 0 ||
      posix_spawn_file_actions_adddup2(&actions, null_fd, STDIN_FILENO) != 0 ||
      posix_spawn_file_actions_adddup2(
          &actions, capture_stdout ? log_fd : null_fd, STDOUT_FILENO) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO) != 0) {
    if (log_fd >= 0) (void)::close(log_fd);
    if (null_fd >= 0) (void)::close(null_fd);
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (error != nullptr) *error = "prepare child process stdio failed";
    return -1;
  }
  pid_t pid = -1;
  const int spawn_result = posix_spawn(&pid, arguments[0].c_str(), &actions,
                                       &attributes, argv.data(), environ);
  (void)posix_spawnattr_destroy(&attributes);
  (void)posix_spawn_file_actions_destroy(&actions);
  (void)::close(log_fd);
  (void)::close(null_fd);
  if (spawn_result != 0) {
    if (error != nullptr) *error = "posix_spawn child process failed: " +
                                  std::string(std::strerror(spawn_result));
    return -1;
  }
  const std::string process_name = fs::path(arguments[0]).filename().string();
  return WaitForProcessWithDeadline(pid, log_path, timeout_ms,
                                    terminate_grace_ms, process_name, error);
}

bool ParseSingleFrameCount(const fs::path& path, uint64_t* count) {
  if (count == nullptr) return false;
  std::ifstream input(path);
  std::string token;
  std::string extra;
  if (!(input >> token) || (input >> extra) || token.empty() || token.front() == '-') {
    return false;
  }
  size_t consumed = 0U;
  try {
    const uint64_t value = std::stoull(token, &consumed, 10);
    if (consumed != token.size()) return false;
    *count = value;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

const char* H264Mp4FinishOutcomeName(H264Mp4FinishOutcome outcome) noexcept {
  switch (outcome) {
    case H264Mp4FinishOutcome::kPublishedComplete: return "published_complete";
    case H264Mp4FinishOutcome::kPublishedPartial: return "published_partial";
    case H264Mp4FinishOutcome::kAborted: return "aborted";
    case H264Mp4FinishOutcome::kCleanupIncomplete: return "cleanup_incomplete";
  }
  return "unknown";
}

class H264Mp4Recorder::Impl {
 public:
  struct EncodedJob {
    int camera_id = 0;
    uint64_t timestamp_ns = 0U;
    bool key_frame = false;
    std::vector<uint8_t> payload;
  };

  void Start(const Options& options, const std::string& output_directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("H264 MP4 recorder already started");
    }
    const fs::path requested_path =
        TrimTrailingSeparators(fs::path(output_directory));
    const std::string requested_path_text = requested_path.string();
    if (requested_path_text.empty() || requested_path_text.front() != '/' ||
        requested_path.filename().empty() ||
        HasReservedPartialSuffix(requested_path) ||
        options.video_codec != VideoCodec::kH264 || options.camera_mask == 0U ||
        options.width <= 0 || options.height <= 0 || options.fps <= 0 ||
        options.bps <= 0) {
      throw std::invalid_argument("invalid H264 MP4 recorder options");
    }
    output_path_ = ResolveH264Mp4OutputPath(requested_path);
    partial_path_ = fs::path(output_path_.string() + ".partial");
    ffmpeg_path_ = FindExecutable("ffmpeg");
    if (ffmpeg_path_.empty()) {
      throw std::runtime_error("ffmpeg executable not found in PATH");
    }
    ffprobe_path_ = FindExecutable("ffprobe");
    if (ffprobe_path_.empty()) {
      throw std::runtime_error("ffprobe executable not found in PATH");
    }
    camera_mask_ = options.camera_mask;
    width_ = options.width;
    height_ = options.height;
    fps_ = options.fps;
    bitrate_kbps_ = options.bps;
    publication_id_ = GeneratePublicationId();
    stats_ = H264Mp4RecorderStats{};
    source_health_ = H264Mp4SourceHealth{};
    source_health_set_ = false;
    stats_.encoded_queue_capacity = encoded_queue_capacity_;
    stats_.encoded_queue_byte_capacity = encoded_queue_byte_capacity_;
    const fs::path parent = output_path_.parent_path();
    fs::create_directories(parent);
    staging_path_ = parent /
        ("." + output_path_.filename().string() + ".tmp-" + std::to_string(::getpid()));
    if (fs::exists(staging_path_)) {
      throw std::runtime_error("H264 MP4 staging directory already exists");
    }
    fs::create_directory(staging_path_);
    try {
      {
        std::ofstream marker(staging_path_ / kPublicationIncompleteMarker, std::ios::trunc);
        marker << "robobaton_h264_mp4_publication_incomplete_v1\n";
        if (!FlushAndClose(&marker)) {
          throw std::runtime_error("write publication incomplete marker failed");
        }
      }
      for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
        resync_required_[camera_id] = true;
        if (!CameraEnabled(camera_mask_, camera_id)) continue;
        raw_files_[camera_id].open(
            staging_path_ / ("camera" + std::to_string(camera_id) + ".h264"),
            std::ios::binary | std::ios::trunc);
        timestamp_files_[camera_id].open(
            staging_path_ /
                ("camera" + std::to_string(camera_id) + "_timestamps.csv"),
            std::ios::trunc);
        if (!raw_files_[camera_id] || !timestamp_files_[camera_id]) {
          throw std::runtime_error("open H264 camera output failed");
        }
        timestamp_files_[camera_id]
            << "frame_index,timestamp_ns,key_frame,encoded_bytes\n";
      }
      imu_file_.open(staging_path_ / "imu.csv", std::ios::trunc);
      if (!imu_file_) {
        throw std::runtime_error("open H264 MP4 IMU CSV failed");
      }
      WriteImuHeader(imu_file_);
      WriteCameraParameters(staging_path_ / "camera_params.yaml", camera_mask_,
                            width_, height_);
      started_ = true;
      worker_ = std::thread(&Impl::WorkerEntry, this);
    } catch (...) {
      static_cast<void>(CloseOutputs());
      std::error_code ignored;
      fs::remove_all(staging_path_, ignored);
      throw;
    }
  }

  void ObserveEncodedFrame(int camera_id,
                           const prrtsp_encoded_frame_v2& frame) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_ || stopping_ || !CameraEnabled(camera_mask_, camera_id)) return;
    if (frame.struct_size < PRRTSP_ENCODED_FRAME_V2_0_SIZE ||
        frame.codec != PRRTSP_CODEC_H264 || frame.data_address == 0U ||
        frame.size_bytes == 0U || frame.size_bytes > kMaxEncodedFrameBytes ||
        (frame.flags & ~PRRTSP_ENCODED_FRAME_FLAG_KEY_FRAME) != 0U ||
        frame.data_address > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("invalid encoded H264 frame");
      return;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(frame.data_address));
    H264AccessUnitInfo access_unit;
    if (!ParseH264AccessUnit(data, static_cast<size_t>(frame.size_bytes),
                             &access_unit)) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("invalid Annex-B H264 access unit");
      return;
    }
    try {
      if (access_unit.has_sps) {
        cached_sps_[camera_id].assign(data + access_unit.sps_begin,
                                      data + access_unit.sps_end);
      }
      if (access_unit.has_pps) {
        cached_pps_[camera_id].assign(data + access_unit.pps_begin,
                                      data + access_unit.pps_end);
      }
    } catch (...) {
      SetFatalLocked("cache H264 parameter set failed");
      return;
    }
    if (!access_unit.has_vcl) {
      return;
    }
    ++stats_.encoded_frames_selected;
    ++stats_.encoded_frames_selected_by_camera[camera_id];
    if (frame.timestamp_ns == 0U) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("invalid encoded H264 timestamp");
      return;
    }
    if (last_seen_timestamp_ns_[camera_id] != 0U &&
        frame.timestamp_ns <= last_seen_timestamp_ns_[camera_id]) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("encoded H264 timestamp is not strictly increasing");
      return;
    }
    last_seen_timestamp_ns_[camera_id] = frame.timestamp_ns;
    const bool key_frame = access_unit.has_idr;
    if (resync_required_[camera_id] && !key_frame) {
      DropEncodedFrameLocked(camera_id, false, false);
      return;
    }
    const bool prepend_parameter_sets =
        resync_required_[camera_id] && key_frame &&
        (!access_unit.has_sps || !access_unit.has_pps);
    if (prepend_parameter_sets &&
        (cached_sps_[camera_id].empty() || cached_pps_[camera_id].empty())) {
      DropEncodedFrameLocked(camera_id, false, false);
      return;
    }
    const uint64_t parameter_set_bytes = prepend_parameter_sets
        ? cached_sps_[camera_id].size() + cached_pps_[camera_id].size()
        : 0U;
    if (parameter_set_bytes > kMaxEncodedFrameBytes - frame.size_bytes) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("H264 access unit with parameter sets is too large");
      return;
    }
    const uint64_t frame_size = frame.size_bytes + parameter_set_bytes;
    if (encoded_queue_.size() >= encoded_queue_capacity_) {
      DropEncodedFrameLocked(camera_id, true, false);
      resync_required_[camera_id] = true;
      return;
    }
    if (frame_size > encoded_queue_byte_capacity_ ||
        encoded_queue_current_bytes_ >
            encoded_queue_byte_capacity_ - frame_size) {
      DropEncodedFrameLocked(camera_id, false, true);
      resync_required_[camera_id] = true;
      return;
    }
    EncodedJob job;
    job.camera_id = camera_id;
    job.timestamp_ns = frame.timestamp_ns;
    job.key_frame = key_frame;
    try {
      job.payload.reserve(static_cast<size_t>(frame_size));
      if (prepend_parameter_sets) {
        job.payload.insert(job.payload.end(), cached_sps_[camera_id].begin(),
                           cached_sps_[camera_id].end());
        job.payload.insert(job.payload.end(), cached_pps_[camera_id].begin(),
                           cached_pps_[camera_id].end());
      }
      job.payload.insert(job.payload.end(), data,
                         data + static_cast<size_t>(frame.size_bytes));
      encoded_queue_.push_back(std::move(job));
    } catch (...) {
      DropEncodedFrameLocked(camera_id, false, false);
      SetFatalLocked("copy encoded H264 frame failed");
      return;
    }
    encoded_queue_current_bytes_ += frame_size;
    resync_required_[camera_id] = false;
    ++stats_.encoded_frames_admitted;
    ++stats_.encoded_frames_admitted_by_camera[camera_id];
    RefreshEncodedQueueStatsLocked();
    if (encoded_queue_.size() > stats_.encoded_queue_peak_depth) {
      stats_.encoded_queue_peak_depth = encoded_queue_.size();
    }
    lock.unlock();
    condition_.notify_one();
  }

  void ObserveImu(const icm42688_sample_t& sample) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_ || stopping_) return;
    if (sample.struct_size < sizeof(icm42688_sample_t) ||
        sample.sample_timestamp_ns == 0U ||
        (last_imu_timestamp_ns_ != 0U &&
         sample.sample_timestamp_ns <= last_imu_timestamp_ns_)) {
      ++stats_.imu_samples_dropped;
      // producer 已独立记录无效/重复/回退；recorder 将其归为可发布的显式 partial。
      return;
    }
    last_imu_timestamp_ns_ = sample.sample_timestamp_ns;
    if (imu_queue_.size() >= kImuQueueCapacity) {
      ++stats_.imu_samples_dropped;
      return;
    }
    try {
      imu_queue_.push_back(sample);
    } catch (...) {
      ++stats_.imu_samples_dropped;
      SetFatalLocked("copy IMU sample failed");
      return;
    }
    ++stats_.imu_samples_admitted;
    if (imu_queue_.size() > stats_.imu_queue_peak_depth) {
      stats_.imu_queue_peak_depth = imu_queue_.size();
    }
    lock.unlock();
    condition_.notify_one();
  }

  void SetSourceHealth(const H264Mp4SourceHealth& health) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) return;
    source_health_ = health;
    source_health_set_ = true;
  }

  H264Mp4FinishResult Finish(bool session_success) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finished_) return finish_result_;
      if (!started_) {
        finished_ = true;
        finish_result_.outcome = H264Mp4FinishOutcome::kAborted;
        finish_result_.error = "H264 MP4 recorder was not started";
        return finish_result_;
      }
      stopping_ = true;
      writer_paused_ = false;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    const bool outputs_closed = CloseOutputs();
    if (!outputs_closed) {
      std::lock_guard<std::mutex> lock(mutex_);
      SetFatalLocked("close H264 MP4 outputs failed");
    }

    bool remux_ok = outputs_closed;
    if (outputs_closed) {
      for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
        if (!CameraEnabled(camera_mask_, camera_id)) continue;
        if (written_by_camera_[camera_id] == 0U ||
            !RemuxCamera(camera_id)) {
          remux_ok = false;
        }
      }
    }

    H264Mp4FinishResult result;
    H264Mp4RecorderStats stats;
    H264Mp4SourceHealth source_health;
    bool source_health_set = false;
    std::string fatal_error;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stats = stats_;
      source_health = source_health_;
      source_health_set = source_health_set_;
      fatal_error = error_;
    }
    bool camera_counts_complete = true;
    bool first_enabled_camera = true;
    uint64_t expected_camera_frames = 0U;
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (!CameraEnabled(camera_mask_, camera_id)) continue;
      const uint64_t selected = stats.encoded_frames_selected_by_camera[camera_id];
      const uint64_t admitted = stats.encoded_frames_admitted_by_camera[camera_id];
      const uint64_t written = stats.encoded_frames_written_by_camera[camera_id];
      if (selected == 0U || selected != admitted || admitted != written) {
        camera_counts_complete = false;
      }
      if (first_enabled_camera) {
        expected_camera_frames = written;
        first_enabled_camera = false;
      } else if (written != expected_camera_frames) {
        camera_counts_complete = false;
      }
    }
    const bool source_health_complete = source_health_set &&
        source_health.samples > 0U &&
        source_health.samples == stats.imu_samples_admitted &&
        source_health.invalid_samples == 0U &&
        source_health.timestamp_duplicates == 0U &&
        source_health.timestamp_regressions == 0U &&
        source_health.sequence_gaps == 0U &&
        source_health.sequence_duplicates == 0U &&
        source_health.sequence_regressions == 0U &&
        source_health.gpio_event_gap_count == 0U &&
        source_health.fifo_overflow_count == 0U &&
        source_health.timing_sample_drops == 0U &&
        source_health.uncertainty_over_200_drops == 0U &&
        source_health.producer_final_health_valid &&
        source_health.producer_session_generation != 0U &&
        source_health.producer_published_samples == source_health.samples &&
        source_health.max_timestamp_uncertainty_us <=
            kMaxCompleteImuTimestampUncertaintyUs &&
        source_health.mapper_counter_valid;
    const bool data_complete = session_success && remux_ok && fatal_error.empty() &&
        camera_counts_complete && source_health_complete &&
        stats.encoded_frames_dropped == 0U && stats.imu_samples_dropped == 0U &&
        stats.encoded_frames_admitted == stats.encoded_frames_written &&
        stats.imu_samples_written > 0U &&
        stats.imu_samples_admitted == stats.imu_samples_written;
    if (data_complete) {
      result.outcome = H264Mp4FinishOutcome::kPublishedComplete;
    } else if (session_success && remux_ok) {
      result.outcome = H264Mp4FinishOutcome::kPublishedPartial;
    } else {
      result.outcome = H264Mp4FinishOutcome::kAborted;
    }
    result.data_complete = data_complete;
    result.error = fatal_error;
    if (!remux_ok && result.error.empty()) result.error = "one or more H264 remux operations failed";
    if (!session_success && result.error.empty()) result.error = "sensor session failed";

    const fs::path destination = data_complete ? output_path_ : partial_path_;
    const bool status_ok = WriteSessionStatus(
        staging_path_ / "session_status.json", result);
    bool durable = status_ok && SyncStagingFiles();
    bool renamed_to_destination = false;
    std::error_code rename_error;
    if (durable) {
      (void)RenameNoReplace(staging_path_, destination, &rename_error);
      if (!rename_error) {
        renamed_to_destination = true;
        result.published_path = destination.string();
        std::string sync_error;
        bool inject_parent_sync_failure = false;
#ifdef RELEASE008_TESTING
        inject_parent_sync_failure = fail_parent_sync_after_rename_;
#endif
        durable = !inject_parent_sync_failure &&
                  FsyncDirectory(destination.parent_path(), &sync_error);
        if (inject_parent_sync_failure) {
          sync_error = "publish parent directory sync failed (injected)";
        }
        if (!durable && result.error.empty()) result.error = sync_error;
      }
    }
    if (durable && renamed_to_destination) {
      durable = WritePublicationReceipt(destination, result);
      if (durable) {
        // 数据、状态和收据均已落盘；marker 删除是唯一提交动作。
        std::error_code marker_error;
#ifdef RELEASE008_TESTING
        if (fail_marker_removal_) {
          marker_error = std::make_error_code(std::errc::io_error);
        } else {
          fs::remove(destination / kPublicationIncompleteMarker, marker_error);
        }
#else
        fs::remove(destination / kPublicationIncompleteMarker, marker_error);
#endif
        durable = !marker_error;
        if (!durable && result.error.empty()) {
          result.error = marker_error.message();
        }
      }
      if (!durable && result.error.empty()) {
        result.error = "publication receipt durability failed";
      }
    }
    if (!durable && data_complete && renamed_to_destination) {
      // rename 后任何 durability 失败都必须退出普通 final 路径。
      std::error_code exists_error;
      const bool quarantine_exists = fs::exists(partial_path_, exists_error);
      std::error_code quarantine_error;
      if (!exists_error && !quarantine_exists) {
#ifdef RELEASE008_TESTING
        if (fail_cleanup_and_quarantine_) {
          quarantine_error = std::make_error_code(std::errc::io_error);
        } else {
          (void)RenameNoReplace(destination, partial_path_, &quarantine_error);
        }
#else
        (void)RenameNoReplace(destination, partial_path_, &quarantine_error);
#endif
      }
      if (!exists_error && !quarantine_exists && !quarantine_error) {
        result.published_path = partial_path_.string();
        std::string sync_error;
        if (!FsyncDirectory(partial_path_.parent_path(), &sync_error) &&
            result.error.empty()) {
          result.error = sync_error;
        }
      } else if (result.error.empty()) {
        result.error = exists_error
                           ? exists_error.message()
                           : (quarantine_exists
                                  ? "partial quarantine path already exists"
                                  : quarantine_error.message());
      }
    }
    if (!durable || rename_error) {
      result.outcome = H264Mp4FinishOutcome::kCleanupIncomplete;
      result.cleanup_complete = false;
      result.data_complete = false;
      if (result.error.empty()) {
        result.error = rename_error ? rename_error.message() : "publish durability failed";
      }
    }
    if (!durable && renamed_to_destination &&
        result.published_path == partial_path_.string()) {
      // 隔离目录先撤销已接受的收据和旧状态，再发布与返回值一致的失败状态。
      std::string downgrade_error;
      if (!RewriteSessionStatus(partial_path_, result, &downgrade_error)) {
        AppendFinishError(&result.error, "session status downgrade failed");
        AppendFinishError(&result.error, downgrade_error);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finish_result_ = result;
      finished_ = true;
      started_ = false;
    }
    return result;
  }

  bool HasFatalError() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !error_.empty();
  }

  std::string ErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  H264Mp4RecorderStats SnapshotStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

#ifdef RELEASE008_TESTING
  void PauseWriterForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    writer_paused_ = true;
  }

  void ResumeWriterForTest() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      writer_paused_ = false;
    }
    condition_.notify_all();
  }

  void SetEncodedQueueCapacityForTest(std::size_t capacity) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoded_queue_.empty() && capacity > 0U &&
        capacity <= kEncodedQueueCapacity) {
      encoded_queue_capacity_ = capacity;
      RefreshEncodedQueueStatsLocked();
    }
  }

  void SetEncodedQueueByteCapacityForTest(uint64_t byte_capacity) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoded_queue_.empty() && byte_capacity > 0U &&
        byte_capacity <= kEncodedQueueByteCapacity) {
      encoded_queue_byte_capacity_ = byte_capacity;
      RefreshEncodedQueueStatsLocked();
    }
  }

  void SetRemuxTimeoutForTest(uint32_t timeout_ms,
                              uint32_t terminate_grace_ms) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timeout_ms > 0U && terminate_grace_ms > 0U) {
      remux_timeout_override_ms_ = timeout_ms;
      remux_terminate_grace_ms_ = terminate_grace_ms;
    }
  }

  uint32_t RemuxTimeoutForBytesForTest(uint64_t raw_bytes) const noexcept {
    return RemuxTimeoutForBytes(raw_bytes);
  }

  void FailPublicationReceiptAfterRenameForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_receipt_after_rename_ = true;
  }

  void FailPublicationCleanupAndQuarantineForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_cleanup_and_quarantine_ = true;
  }

  void FailParentDirectorySyncAfterRenameForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_parent_sync_after_rename_ = true;
  }
  void FailPublicationMarkerRemovalForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_marker_removal_ = true;
  }

  void FailSessionStatusRewriteWriteForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_status_rewrite_write_ = true;
  }

  void FailSessionStatusRewriteFsyncForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_status_rewrite_fsync_ = true;
  }

  void FailSessionStatusRewriteRenameForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_status_rewrite_rename_ = true;
  }

  void FailSessionStatusRewriteDirectorySyncForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_status_rewrite_directory_sync_ = true;
  }


  void FailSessionStatusCloseForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_session_status_close_ = true;
  }

  void FailPublicationReceiptCloseForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_receipt_close_ = true;
  }

  void FailOutputCloseForTest() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_output_close_ = true;
  }
#endif

 private:
  void SetFatalLocked(const std::string& message) noexcept {
    if (error_.empty()) error_ = message;
    condition_.notify_all();
  }

  void RefreshEncodedQueueStatsLocked() noexcept {
    stats_.encoded_queue_capacity = encoded_queue_capacity_;
    stats_.encoded_queue_current_depth = encoded_queue_.size();
    if (stats_.encoded_queue_current_depth > stats_.encoded_queue_peak_depth) {
      stats_.encoded_queue_peak_depth = stats_.encoded_queue_current_depth;
    }
    stats_.encoded_queue_byte_capacity = encoded_queue_byte_capacity_;
    stats_.encoded_queue_current_bytes = encoded_queue_current_bytes_;
    if (encoded_queue_current_bytes_ > stats_.encoded_queue_byte_high_watermark) {
      stats_.encoded_queue_byte_high_watermark = encoded_queue_current_bytes_;
    }
  }

  void DropEncodedFrameLocked(int camera_id, bool count_full,
                              bool byte_full) noexcept {
    ++stats_.encoded_frames_dropped;
    if (camera_id >= 0 && camera_id < kMaxChannels) {
      ++stats_.encoded_frames_dropped_by_camera[camera_id];
    }
    if (count_full) ++stats_.encoded_queue_full_drops;
    if (byte_full) ++stats_.encoded_queue_byte_full_drops;
  }

  void RemoveQueuedBytesLocked(uint64_t byte_count) noexcept {
    if (encoded_queue_current_bytes_ >= byte_count) {
      encoded_queue_current_bytes_ -= byte_count;
    } else {
      encoded_queue_current_bytes_ = 0U;
    }
    RefreshEncodedQueueStatsLocked();
  }

  void DropQueuedFramesLocked() noexcept {
    for (const EncodedJob& job : encoded_queue_) {
      DropEncodedFrameLocked(job.camera_id, false, false);
    }
    encoded_queue_.clear();
    encoded_queue_current_bytes_ = 0U;
    RefreshEncodedQueueStatsLocked();
  }

  void WorkerEntry() noexcept {
    while (true) {
      EncodedJob frame;
      std::array<icm42688_sample_t, kImuWriteBatch> imu_batch{};
      size_t imu_count = 0U;
      bool have_frame = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
          return stopping_ || !error_.empty() ||
                 (!writer_paused_ &&
                  (!encoded_queue_.empty() || !imu_queue_.empty()));
        });
        if (!error_.empty()) {
          DropQueuedFramesLocked();
          stats_.imu_samples_dropped += imu_queue_.size();
          imu_queue_.clear();
          break;
        }
        if (!encoded_queue_.empty()) {
          const uint64_t queued_bytes = encoded_queue_.front().payload.size();
          frame = std::move(encoded_queue_.front());
          encoded_queue_.pop_front();
          RemoveQueuedBytesLocked(queued_bytes);
          have_frame = true;
        }
        while (imu_count < imu_batch.size() && !imu_queue_.empty()) {
          imu_batch[imu_count++] = imu_queue_.front();
          imu_queue_.pop_front();
        }
        if (!have_frame && imu_count == 0U && stopping_) break;
      }

      if (have_frame && !WriteEncodedFrame(frame)) {
        std::lock_guard<std::mutex> lock(mutex_);
        DropEncodedFrameLocked(frame.camera_id, false, false);
        SetFatalLocked("write encoded H264 stream failed");
        continue;
      }
      for (size_t index = 0U; index < imu_count; ++index) {
        WriteImuRow(imu_file_, imu_batch[index]);
        if (!imu_file_) {
          std::lock_guard<std::mutex> lock(mutex_);
          SetFatalLocked("write IMU CSV failed");
          break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.imu_samples_written;
      }
    }
  }

  bool WriteEncodedFrame(const EncodedJob& job) {
    std::ofstream& raw = raw_files_[job.camera_id];
    std::ofstream& timestamps = timestamp_files_[job.camera_id];
    raw.write(reinterpret_cast<const char*>(job.payload.data()),
              static_cast<std::streamsize>(job.payload.size()));
    if (!raw) return false;
    const uint64_t frame_index = written_by_camera_[job.camera_id];
    timestamps << frame_index << ',' << job.timestamp_ns << ','
               << (job.key_frame ? 1 : 0) << ',' << job.payload.size() << '\n';
    if (!timestamps) return false;
    if (first_timestamp_by_camera_[job.camera_id] == 0U) {
      first_timestamp_by_camera_[job.camera_id] = job.timestamp_ns;
    }
    ++written_by_camera_[job.camera_id];
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.encoded_frames_written;
    ++stats_.encoded_frames_written_by_camera[job.camera_id];
    stats_.encoded_bytes_written += job.payload.size();
    return true;
  }

  bool CloseOutputs() noexcept {
    bool success = true;
    for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
      if (raw_files_[camera_id].is_open()) {
        raw_files_[camera_id].flush();
        success = static_cast<bool>(raw_files_[camera_id]) && success;
        raw_files_[camera_id].close();
        success = static_cast<bool>(raw_files_[camera_id]) && success;
      }
      if (timestamp_files_[camera_id].is_open()) {
        timestamp_files_[camera_id].flush();
        success = static_cast<bool>(timestamp_files_[camera_id]) && success;
        timestamp_files_[camera_id].close();
        success = static_cast<bool>(timestamp_files_[camera_id]) && success;
      }
    }
    if (imu_file_.is_open()) {
      imu_file_.flush();
      success = static_cast<bool>(imu_file_) && success;
      imu_file_.close();
      success = static_cast<bool>(imu_file_) && success;
    }
#ifdef RELEASE008_TESTING
    success = !fail_output_close_ && success;
#endif
    return success;
  }

  bool RemuxCamera(int camera_id) noexcept {
    const std::string prefix = "camera" + std::to_string(camera_id);
    const fs::path raw = staging_path_ / (prefix + ".h264");
    const fs::path temporary_mp4 = staging_path_ / ("." + prefix + ".mp4.tmp");
    const fs::path final_mp4 = staging_path_ / (prefix + ".mp4");
    const fs::path log = staging_path_ / (prefix + "_ffmpeg.log");
    std::error_code size_error;
    const uint64_t raw_bytes = fs::file_size(raw, size_error);
    if (size_error) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = "read raw H264 size failed: " + size_error.message();
      return false;
    }
    const uint32_t remux_timeout_ms = remux_timeout_override_ms_ != 0U
                                          ? remux_timeout_override_ms_
                                          : RemuxTimeoutForBytes(raw_bytes);
    std::vector<std::string> arguments = {
        ffmpeg_path_, "-v", "error", "-fflags", "+genpts", "-r",
        std::to_string(fps_), "-f", "h264", "-i", raw.string(), "-an",
        "-c:v", "copy", "-movflags",
        "use_metadata_tags+empty_moov+default_base_moof+frag_keyframe",
        "-metadata", "start_abs_timestamp_ns=" +
            std::to_string(first_timestamp_by_camera_[camera_id]),
        "-metadata", "timestamp_index=" + prefix + "_timestamps.csv",
        "-metadata", "frame_rate=" + std::to_string(fps_), "-f", "mp4", "-y",
        temporary_mp4.string()};
    std::string process_error;
    if (RunProcess(arguments, log, remux_timeout_ms,
                   remux_terminate_grace_ms_, false, &process_error) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = process_error;
      return false;
    }
    const fs::path probe_log = staging_path_ / (prefix + "_ffprobe.log");
    const std::vector<std::string> probe_arguments = {
        ffprobe_path_, "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=nb_read_frames", "-of",
        "default=noprint_wrappers=1:nokey=1", temporary_mp4.string()};
    if (RunProcess(probe_arguments, probe_log, remux_timeout_ms,
                   remux_terminate_grace_ms_, true, &process_error) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = process_error;
      return false;
    }
    uint64_t decoded_frames = 0U;
    const bool frame_count_valid = ParseSingleFrameCount(probe_log, &decoded_frames);
    if (!frame_count_valid || decoded_frames != written_by_camera_[camera_id]) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) {
        error_ = "ffprobe frame count mismatch for " + prefix + ": expected " +
                 std::to_string(written_by_camera_[camera_id]) + ", got " +
                 (frame_count_valid ? std::to_string(decoded_frames) : "invalid");
      }
      return false;
    }
    std::error_code error;
    (void)RenameNoReplace(temporary_mp4, final_mp4, &error);
    if (error) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = "publish camera MP4 failed: " + error.message();
      return false;
    }
    fs::remove(raw, error);
    if (error) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = "remove remuxed H264 source failed: " + error.message();
      return false;
    }
    fs::remove(log, error);
    if (error) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = "remove ffmpeg log failed: " + error.message();
      return false;
    }
    fs::remove(probe_log, error);
    if (error) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = "remove ffprobe log failed: " + error.message();
      return false;
    }
    return true;
  }

  bool WriteSessionStatus(const fs::path& status_path,
                          const H264Mp4FinishResult& preliminary) noexcept {
    try {
      const H264Mp4RecorderStats stats = SnapshotStats();
      H264Mp4SourceHealth source_health;
      bool source_health_set = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        source_health = source_health_;
        source_health_set = source_health_set_;
      }
      std::ofstream output(status_path, std::ios::trunc);
      if (!output) return false;
      output << "{\n"
             << "  \"schema\": \"" << kSessionSchema << "\",\n"
             << "  \"publication_id\": \"" << publication_id_ << "\",\n"
             << "  \"outcome\": \""
             << H264Mp4FinishOutcomeName(preliminary.outcome) << "\",\n"
             << "  \"data_complete\": "
             << (preliminary.data_complete ? "true" : "false") << ",\n"
             << "  \"camera_mask\": " << camera_mask_ << ",\n"
             << "  \"encoded_frames_selected\": " << stats.encoded_frames_selected << ",\n"
             << "  \"encoded_frames_admitted\": " << stats.encoded_frames_admitted << ",\n"
             << "  \"encoded_frames_written\": " << stats.encoded_frames_written << ",\n"
             << "  \"encoded_frames_dropped\": " << stats.encoded_frames_dropped << ",\n"
             << "  \"encoded_frames_selected_by_camera\": ";
      WriteCameraCounterObject(output, stats.encoded_frames_selected_by_camera);
      output << ",\n"
             << "  \"encoded_frames_admitted_by_camera\": ";
      WriteCameraCounterObject(output, stats.encoded_frames_admitted_by_camera);
      output << ",\n"
             << "  \"encoded_frames_written_by_camera\": ";
      WriteCameraCounterObject(output, stats.encoded_frames_written_by_camera);
      output << ",\n"
             << "  \"encoded_frames_dropped_by_camera\": ";
      WriteCameraCounterObject(output, stats.encoded_frames_dropped_by_camera);
      output << ",\n"
             << "  \"encoded_queue_capacity\": " << stats.encoded_queue_capacity << ",\n"
             << "  \"encoded_queue_current_depth\": "
             << stats.encoded_queue_current_depth << ",\n"
             << "  \"encoded_queue_peak_depth\": " << stats.encoded_queue_peak_depth << ",\n"
             << "  \"encoded_queue_full_drops\": " << stats.encoded_queue_full_drops << ",\n"
             << "  \"encoded_queue_byte_capacity\": "
             << stats.encoded_queue_byte_capacity << ",\n"
             << "  \"encoded_queue_current_bytes\": "
             << stats.encoded_queue_current_bytes << ",\n"
             << "  \"encoded_queue_byte_high_watermark\": "
             << stats.encoded_queue_byte_high_watermark << ",\n"
             << "  \"encoded_queue_byte_full_drops\": "
             << stats.encoded_queue_byte_full_drops << ",\n"
             << "  \"encoded_bytes_written\": " << stats.encoded_bytes_written << ",\n"
             << "  \"imu_samples_admitted\": " << stats.imu_samples_admitted << ",\n"
             << "  \"imu_samples_written\": " << stats.imu_samples_written << ",\n"
             << "  \"imu_samples_dropped\": " << stats.imu_samples_dropped << ",\n"
             << "  \"imu_source_health_set\": "
             << (source_health_set ? "true" : "false") << ",\n"
             << "  \"imu_source_samples\": " << source_health.samples << ",\n"
             << "  \"imu_source_invalid_samples\": "
             << source_health.invalid_samples << ",\n"
             << "  \"imu_source_timestamp_duplicates\": "
             << source_health.timestamp_duplicates << ",\n"
             << "  \"imu_source_timestamp_regressions\": "
             << source_health.timestamp_regressions << ",\n"
             << "  \"imu_source_sequence_gaps\": "
             << source_health.sequence_gaps << ",\n"
             << "  \"imu_source_sequence_duplicates\": "
             << source_health.sequence_duplicates << ",\n"
             << "  \"imu_source_sequence_regressions\": "
             << source_health.sequence_regressions << ",\n"
             << "  \"imu_source_gpio_event_gap_count\": "
             << source_health.gpio_event_gap_count << ",\n"
             << "  \"imu_source_fifo_overflow_count\": "
             << source_health.fifo_overflow_count << ",\n"
             << "  \"imu_source_timing_sample_drops\": "
             << source_health.timing_sample_drops << ",\n"
             << "  \"imu_source_max_timestamp_uncertainty_us\": "
             << source_health.max_timestamp_uncertainty_us << ",\n"
             << "  \"imu_source_max_consecutive_drops\": "
             << source_health.max_consecutive_drops << ",\n"
             << "  \"imu_source_uncertainty_over_200_drops\": "
             << source_health.uncertainty_over_200_drops << ",\n"
             << "  \"imu_source_producer_session_generation\": "
             << source_health.producer_session_generation << ",\n"
             << "  \"imu_source_producer_published_samples\": "
             << source_health.producer_published_samples << ",\n"
             << "  \"imu_source_producer_final_health_valid\": "
             << (source_health.producer_final_health_valid ? "true" : "false") << ",\n"
             << "  \"imu_source_mapper_counter_valid\": "
             << (source_health.mapper_counter_valid ? "true" : "false") << ",\n"
             << "  \"error\": \"" << JsonEscape(preliminary.error) << "\"\n"
             << "}\n";
      bool closed = FlushAndClose(&output);
#ifdef RELEASE008_TESTING
      closed = !fail_session_status_close_ && closed;
#endif
      return closed;
    } catch (...) {
      return false;
    }
  }
  // 隔离目录必须先撤销 complete 元数据，再以原子替换发布失败状态。
  bool RewriteSessionStatus(const fs::path& directory,
                            const H264Mp4FinishResult& final_result,
                            std::string* failure) noexcept {
    const fs::path temporary = directory / ".session_status.json.tmp";
    const fs::path status = directory / "session_status.json";
    const fs::path failed_status = directory / ".session_status.failed";
    const fs::path receipt = directory / "publication_receipt.json";
    const fs::path failed_receipt = directory / ".publication_receipt.failed";
    const auto remove_temporary = [&temporary]() noexcept {
      std::error_code ignored;
      fs::remove(temporary, ignored);
    };

    try {
      // 收据必须先退出消费者认可的文件名；失败时保留匹配的旧状态和 marker。
      std::error_code error;
      const bool receipt_exists = fs::exists(receipt, error);
      if (error) {
        AppendFinishError(failure,
                          "inspect publication receipt failed: " + error.message());
        return false;
      }
      if (receipt_exists) {
        (void)RenameNoReplace(receipt, failed_receipt, &error);
        if (error) {
          AppendFinishError(failure,
                            "quarantine publication receipt failed: " +
                                error.message());
          return false;
        }
        std::string sync_error;
        if (!FsyncDirectory(directory, &sync_error)) {
          AppendFinishError(failure,
                            "sync quarantined publication receipt failed: " +
                                sync_error);
          return false;
        }
      }

      // 旧 complete 状态保留为诊断证据，但不能继续占用标准状态文件名。
      error.clear();
      const bool status_exists = fs::exists(status, error);
      if (error || !status_exists) {
        AppendFinishError(
            failure, error ? "inspect session status failed: " + error.message()
                           : "session status missing before downgrade");
        return false;
      }
      (void)RenameNoReplace(status, failed_status, &error);
      if (error) {
        AppendFinishError(failure,
                          "quarantine session status failed: " + error.message());
        return false;
      }
      std::string sync_error;
      if (!FsyncDirectory(directory, &sync_error)) {
        AppendFinishError(failure,
                          "sync quarantined session status failed: " + sync_error);
        return false;
      }

      // 新状态先写同目录临时文件并同步，再用无覆盖 rename 建立唯一标准状态。
#ifdef RELEASE008_TESTING
      if (fail_status_rewrite_write_) {
        AppendFinishError(failure, "write failed (injected)");
        remove_temporary();
        return false;
      }
#endif
      if (!WriteSessionStatus(temporary, final_result)) {
        AppendFinishError(failure, "write failed");
        remove_temporary();
        return false;
      }
#ifdef RELEASE008_TESTING
      if (fail_status_rewrite_fsync_) {
        AppendFinishError(failure, "file sync failed (injected)");
        remove_temporary();
        return false;
      }
#endif
      if (!FsyncFile(temporary, &sync_error)) {
        AppendFinishError(failure, sync_error);
        remove_temporary();
        return false;
      }
      error.clear();
#ifdef RELEASE008_TESTING
      if (fail_status_rewrite_rename_) {
        error = std::make_error_code(std::errc::io_error);
      } else {
        (void)RenameNoReplace(temporary, status, &error);
      }
#else
      (void)RenameNoReplace(temporary, status, &error);
#endif
      if (error) {
        AppendFinishError(failure, "rename failed: " + error.message());
        remove_temporary();
        return false;
      }
#ifdef RELEASE008_TESTING
      if (fail_status_rewrite_directory_sync_) {
        AppendFinishError(failure, "directory sync failed (injected)");
        return false;
      }
#endif
      if (!FsyncDirectory(directory, &sync_error)) {
        AppendFinishError(failure, sync_error);
        return false;
      }
      return true;
    } catch (const std::exception& exception) {
      remove_temporary();
      AppendFinishError(failure, exception.what());
      return false;
    } catch (...) {
      remove_temporary();
      AppendFinishError(failure, "unknown status downgrade failure");
      return false;
    }
  }


  bool WritePublicationReceipt(const fs::path& destination,
                               const H264Mp4FinishResult& final_result) noexcept {
    const fs::path temporary = destination / ".publication_receipt.json.tmp";
    const fs::path receipt = destination / "publication_receipt.json";
    const fs::path failed_receipt = destination / ".publication_receipt.failed";
    try {
      {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << "{\n"
               << "  \"schema\": \"" << kReceiptSchema << "\",\n"
               << "  \"session_schema\": \"" << kSessionSchema << "\",\n"
               << "  \"publication_id\": \"" << publication_id_ << "\",\n"
               << "  \"outcome\": \""
               << H264Mp4FinishOutcomeName(final_result.outcome) << "\",\n"
               << "  \"data_complete\": "
               << (final_result.data_complete ? "true" : "false") << ",\n"
               << "  \"session_status\": \"session_status.json\",\n"
               << "  \"timestamp_indexes\": [";
        bool first = true;
        for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
          if (!CameraEnabled(camera_mask_, camera_id)) continue;
          if (!first) output << ", ";
          first = false;
          output << "\"camera" << camera_id << "_timestamps.csv\"";
        }
        output << "],\n"
               << "  \"mp4_files\": [";
        first = true;
        for (int camera_id = 0; camera_id < kMaxChannels; ++camera_id) {
          if (!CameraEnabled(camera_mask_, camera_id)) continue;
          if (!first) output << ", ";
          first = false;
          output << "\"camera" << camera_id << ".mp4\"";
        }
        output << "]\n"
               << "}\n";
        bool closed = FlushAndClose(&output);
#ifdef RELEASE008_TESTING
        closed = !fail_receipt_close_ && closed;
#endif
        if (!closed) return false;
      }
      if (!FsyncFile(temporary, nullptr)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
      }
      std::error_code error;
      (void)RenameNoReplace(temporary, receipt, &error);
      if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
      }
      bool durable = false;
#ifdef RELEASE008_TESTING
      if (!fail_receipt_after_rename_) {
        std::string sync_error;
        durable = FsyncFile(receipt, &sync_error) &&
                  FsyncDirectory(destination, &sync_error);
      }
#else
      std::string sync_error;
      durable = FsyncFile(receipt, &sync_error) &&
                FsyncDirectory(destination, &sync_error);
#endif
#ifdef RELEASE008_TESTING
      if (fail_cleanup_and_quarantine_) return false;
#endif
      if (durable) return true;

      // A failed receipt must not remain at the externally accepted filename.
      (void)RenameNoReplace(receipt, failed_receipt, &error);
      if (error) {
        error.clear();
        fs::remove(receipt, error);
      }
      std::error_code ignored;
      fs::remove(temporary, ignored);
      std::string ignored_sync_error;
      (void)FsyncDirectory(destination, &ignored_sync_error);
      return false;
    } catch (...) {
      std::error_code ignored;
      fs::remove(temporary, ignored);
      fs::remove(receipt, ignored);
      return false;
    }
  }

  bool SyncStagingFiles() noexcept {
    try {
      std::string error;
      for (const fs::directory_entry& entry : fs::directory_iterator(staging_path_)) {
        if (entry.is_regular_file() && !FsyncFile(entry.path(), &error)) {
          std::lock_guard<std::mutex> lock(mutex_);
          if (error_.empty()) error_ = error;
          return false;
        }
      }
      return FsyncDirectory(staging_path_, &error);
    } catch (const std::exception& exception) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (error_.empty()) error_ = exception.what();
      return false;
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::thread worker_;
  std::deque<EncodedJob> encoded_queue_;
  std::deque<icm42688_sample_t> imu_queue_;
  std::array<std::ofstream, kMaxChannels> raw_files_;
  std::array<std::ofstream, kMaxChannels> timestamp_files_;
  std::array<std::vector<uint8_t>, kMaxChannels> cached_sps_;
  std::array<std::vector<uint8_t>, kMaxChannels> cached_pps_;
  std::ofstream imu_file_;
  std::array<bool, kMaxChannels> resync_required_{};
  std::array<uint64_t, kMaxChannels> last_seen_timestamp_ns_{};
  std::array<uint64_t, kMaxChannels> first_timestamp_by_camera_{};
  std::array<uint64_t, kMaxChannels> written_by_camera_{};
  uint64_t last_imu_timestamp_ns_ = 0U;
  H264Mp4RecorderStats stats_;
  H264Mp4SourceHealth source_health_;
  bool source_health_set_ = false;
  H264Mp4FinishResult finish_result_;
  fs::path output_path_;
  fs::path partial_path_;
  fs::path staging_path_;
  std::string ffmpeg_path_;
  std::string ffprobe_path_;
  std::string publication_id_;
  std::string error_;
  uint32_t camera_mask_ = 0U;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 0;
  long long bitrate_kbps_ = 0;
  std::size_t encoded_queue_capacity_ = kEncodedQueueCapacity;
  uint64_t encoded_queue_byte_capacity_ = kEncodedQueueByteCapacity;
  uint64_t encoded_queue_current_bytes_ = 0U;
  uint32_t remux_timeout_override_ms_ = 0U;
  uint32_t remux_terminate_grace_ms_ = kFfmpegTerminateGraceMs;
  bool started_ = false;
  bool stopping_ = false;
  bool finished_ = false;
  bool writer_paused_ = false;
#ifdef RELEASE008_TESTING
  bool fail_receipt_after_rename_ = false;
  bool fail_cleanup_and_quarantine_ = false;
  bool fail_parent_sync_after_rename_ = false;
  bool fail_marker_removal_ = false;
  bool fail_status_rewrite_write_ = false;
  bool fail_status_rewrite_fsync_ = false;
  bool fail_status_rewrite_rename_ = false;
  bool fail_status_rewrite_directory_sync_ = false;
  bool fail_session_status_close_ = false;
  bool fail_receipt_close_ = false;
  bool fail_output_close_ = false;
#endif
};

H264Mp4Recorder::H264Mp4Recorder() : impl_(std::make_unique<Impl>()) {}

H264Mp4Recorder::~H264Mp4Recorder() {
  if (enabled()) Abort();
}

void H264Mp4Recorder::Start(const Options& options,
                            const std::string& output_directory) {
  impl_->Start(options, output_directory);
  enabled_.store(true, std::memory_order_release);
}

void H264Mp4Recorder::ObserveEncodedFrame(
    int camera_id, const prrtsp_encoded_frame_v2& frame) noexcept {
  if (enabled()) impl_->ObserveEncodedFrame(camera_id, frame);
}

void H264Mp4Recorder::ObserveImu(const icm42688_sample_t& sample) noexcept {
  if (enabled()) impl_->ObserveImu(sample);
}

void H264Mp4Recorder::SetSourceHealth(
    const H264Mp4SourceHealth& health) noexcept {
  if (enabled()) impl_->SetSourceHealth(health);
}

H264Mp4FinishResult H264Mp4Recorder::Finish(bool session_success) noexcept {
  H264Mp4FinishResult result = impl_->Finish(session_success);
  enabled_.store(false, std::memory_order_release);
  return result;
}

void H264Mp4Recorder::Abort() noexcept {
  static_cast<void>(Finish(false));
}

bool H264Mp4Recorder::HasFatalError() const noexcept {
  return impl_->HasFatalError();
}

std::string H264Mp4Recorder::ErrorMessage() const {
  return impl_->ErrorMessage();
}

H264Mp4RecorderStats H264Mp4Recorder::SnapshotStats() const {
  return impl_->SnapshotStats();
}

#ifdef RELEASE008_TESTING
void H264Mp4Recorder::PauseWriterForTest() noexcept {
  impl_->PauseWriterForTest();
}

void H264Mp4Recorder::ResumeWriterForTest() noexcept {
  impl_->ResumeWriterForTest();
}

void H264Mp4Recorder::SetEncodedQueueCapacityForTest(
    std::size_t capacity) noexcept {
  impl_->SetEncodedQueueCapacityForTest(capacity);
}

void H264Mp4Recorder::SetEncodedQueueByteCapacityForTest(
    uint64_t byte_capacity) noexcept {
  impl_->SetEncodedQueueByteCapacityForTest(byte_capacity);
}

void H264Mp4Recorder::SetRemuxTimeoutForTest(
    uint32_t timeout_ms, uint32_t terminate_grace_ms) noexcept {
  impl_->SetRemuxTimeoutForTest(timeout_ms, terminate_grace_ms);
}

uint32_t H264Mp4Recorder::RemuxTimeoutForBytesForTest(
    uint64_t raw_bytes) const noexcept {
  return impl_->RemuxTimeoutForBytesForTest(raw_bytes);
}

void H264Mp4Recorder::FailPublicationReceiptAfterRenameForTest() noexcept {
  impl_->FailPublicationReceiptAfterRenameForTest();
}

void H264Mp4Recorder::FailPublicationCleanupAndQuarantineForTest() noexcept {
  impl_->FailPublicationCleanupAndQuarantineForTest();
}

void H264Mp4Recorder::FailParentDirectorySyncAfterRenameForTest() noexcept {
  impl_->FailParentDirectorySyncAfterRenameForTest();
}

void H264Mp4Recorder::FailPublicationMarkerRemovalForTest() noexcept {
  impl_->FailPublicationMarkerRemovalForTest();
}

void H264Mp4Recorder::FailSessionStatusRewriteWriteForTest() noexcept {
  impl_->FailSessionStatusRewriteWriteForTest();
}

void H264Mp4Recorder::FailSessionStatusRewriteFsyncForTest() noexcept {
  impl_->FailSessionStatusRewriteFsyncForTest();
}

void H264Mp4Recorder::FailSessionStatusRewriteRenameForTest() noexcept {
  impl_->FailSessionStatusRewriteRenameForTest();
}

void H264Mp4Recorder::FailSessionStatusRewriteDirectorySyncForTest() noexcept {
  impl_->FailSessionStatusRewriteDirectorySyncForTest();
}


void H264Mp4Recorder::FailSessionStatusCloseForTest() noexcept {
  impl_->FailSessionStatusCloseForTest();
}

void H264Mp4Recorder::FailPublicationReceiptCloseForTest() noexcept {
  impl_->FailPublicationReceiptCloseForTest();
}

void H264Mp4Recorder::FailOutputCloseForTest() noexcept {
  impl_->FailOutputCloseForTest();
}
#endif

}  // namespace robobaton_demo
