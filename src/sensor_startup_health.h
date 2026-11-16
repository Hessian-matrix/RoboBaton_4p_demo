#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace robobaton_demo {

// The launcher owns the peer socket. Reports come from the capture owner,
// independent of Web availability and log output. Losing the launcher does not
// interrupt acquisition; the descriptor is never inherited by exec children.
class SensorStartupHealth {
 public:
  SensorStartupHealth() {
    const char* value = std::getenv("SENSOR_DEMO_STARTUP_FD");
    if (!value) return;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end || parsed < 3 || parsed > std::numeric_limits<int>::max())
      throw std::runtime_error("invalid startup health descriptor");
    int type = 0;
    socklen_t size = sizeof(type);
    const int fd = static_cast<int>(parsed);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
      throw std::runtime_error("startup health descriptor is not a private packet socket");
    fd_ = fd;
    unsetenv("SENSOR_DEMO_STARTUP_FD");
  }
  ~SensorStartupHealth() { if (fd_ >= 0) close(fd_); }
  SensorStartupHealth(const SensorStartupHealth&) = delete;
  SensorStartupHealth& operator=(const SensorStartupHealth&) = delete;

  void Report(unsigned mask, const std::array<uint64_t, 4>& frames, uint64_t imu) noexcept {
    if (fd_ < 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return;
    next_ = now + std::chrono::milliseconds(250);
    char message[256];
    const int length = std::snprintf(message, sizeof(message), "1 %ld %u %llu %llu %llu %llu %llu",
        static_cast<long>(getpid()), mask,
        static_cast<unsigned long long>(frames[0]), static_cast<unsigned long long>(frames[1]),
        static_cast<unsigned long long>(frames[2]), static_cast<unsigned long long>(frames[3]),
        static_cast<unsigned long long>(imu));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(message) ||
        send(fd_, message, static_cast<size_t>(length), MSG_DONTWAIT | MSG_NOSIGNAL) != length) {
      close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
  std::chrono::steady_clock::time_point next_{};
};
}  // namespace robobaton_demo
