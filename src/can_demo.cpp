#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif

namespace {
volatile sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
using Clock = std::chrono::steady_clock;

struct Options {
  std::string interface = "can0";
  bool transmit = false;
  uint32_t count = 0;
  uint32_t interval_ms = 1000;
  uint32_t timeout_ms = 0;
  can_frame frame{};
};

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw std::invalid_argument("expected hexadecimal digits");
}

uint32_t Number(const std::string& text, unsigned base) {
  size_t start = base == 16 && text.size() >= 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
  if (start == text.size()) throw std::invalid_argument("empty number");
  uint32_t result = 0;
  for (size_t i = start; i < text.size(); ++i) {
    const unsigned digit = static_cast<unsigned>(HexDigit(text[i]));
    if (digit >= base || result > (std::numeric_limits<uint32_t>::max() - digit) / base)
      throw std::invalid_argument("number outside uint32 range or invalid digits");
    result = result * base + digit;
  }
  return result;
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  uint32_t id = 0x123;
  std::string data = "0102030405060708";
  bool extended = false, count_set = false, tx_fields = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) throw std::invalid_argument("missing value for " + arg);
      return argv[i];
    };
    if (arg == "--interface") options.interface = value();
    else if (arg == "--mode") {
      const auto mode = value();
      if (mode != "tx" && mode != "rx") throw std::invalid_argument("mode must be tx or rx");
      options.transmit = mode == "tx";
    } else if (arg == "--id") { id = Number(value(), 16); tx_fields = true; }
    else if (arg == "--extended") { extended = true; tx_fields = true; }
    else if (arg == "--data") { data = value(); tx_fields = true; }
    else if (arg == "--count") { options.count = Number(value(), 10); count_set = true; }
    else if (arg == "--interval-ms") { options.interval_ms = Number(value(), 10); tx_fields = true; }
    else if (arg == "--timeout-ms") options.timeout_ms = Number(value(), 10);
    else throw std::invalid_argument("unknown option: " + arg);
  }
  if (options.interface.empty() || options.interface.size() >= IFNAMSIZ ||
      options.interface.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
    throw std::invalid_argument("invalid CAN interface name");
  if (!options.transmit && tx_fields)
    throw std::invalid_argument("--id/--extended/--data/--interval-ms require --mode tx");
  if (id > (extended ? CAN_EFF_MASK : CAN_SFF_MASK))
    throw std::invalid_argument("CAN ID exceeds selected 11-bit or 29-bit range; use --extended for 29-bit IDs");
  if (data.size() > CAN_MAX_DLEN * 2 || data.size() % 2 != 0)
    throw std::invalid_argument("--data must contain 0..8 bytes as an even number of hex digits");
  if (options.interval_ms == 0 || options.interval_ms > 3600000 || options.timeout_ms > 3600000)
    throw std::invalid_argument("interval must be 1..3600000ms; timeout must be 0..3600000ms");
  options.frame.can_id = id | (extended ? CAN_EFF_FLAG : 0);
  options.frame.len = static_cast<__u8>(data.size() / 2);
  for (size_t i = 0; i < options.frame.len; ++i)
    options.frame.data[i] = static_cast<__u8>(HexDigit(data[2 * i]) * 16 + HexDigit(data[2 * i + 1]));
  if (options.transmit && !count_set) options.count = 1;
  return options;
}

[[noreturn]] void SystemError(const char* operation) {
  const int error = errno;
  throw std::runtime_error(std::string(operation) + ": " + std::strerror(error));
}

class CanSocket {
 public:
  explicit CanSocket(const std::string& interface) {
    fd_ = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd_ < 0) SystemError("socket(CAN_RAW); kernel CAN/RAW support is required");
    try {
      ifreq request{};
      std::memcpy(request.ifr_name, interface.c_str(), interface.size() + 1);
      if (ioctl(fd_, SIOCGIFINDEX, &request) < 0)
        SystemError("CAN interface lookup; check TCAN4550 driver and device tree");
      const int index = request.ifr_ifindex;
      if (ioctl(fd_, SIOCGIFHWADDR, &request) < 0) SystemError("SIOCGIFHWADDR");
      if (request.ifr_hwaddr.sa_family != ARPHRD_CAN)
        throw std::runtime_error("selected interface is not a CAN network device");
      if (ioctl(fd_, SIOCGIFFLAGS, &request) < 0) SystemError("SIOCGIFFLAGS");
      if (!(request.ifr_flags & IFF_UP))
        throw std::runtime_error("CAN interface is down; configure bitrate and bring it up with ip link first");
      sockaddr_can address{};
      address.can_family = AF_CAN;
      address.can_ifindex = index;
      if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) SystemError("bind(CAN_RAW)");
    } catch (...) {
      close(fd_);
      throw;
    }
  }
  ~CanSocket() { close(fd_); }
  CanSocket(const CanSocket&) = delete;
  CanSocket& operator=(const CanSocket&) = delete;
  int get() const { return fd_; }
 private:
  int fd_ = -1;
};

void PrintFrame(const char* direction, const can_frame& frame) {
  const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
  std::cout << direction << ' ' << std::uppercase << std::hex << std::setfill('0')
            << std::setw(extended ? 8 : 3)
            << (frame.can_id & (extended ? CAN_EFF_MASK : CAN_SFF_MASK)) << '#';
  if (frame.can_id & CAN_RTR_FLAG) std::cout << 'R' << std::dec << unsigned(frame.len);
  else for (unsigned i = 0; i < frame.len; ++i) std::cout << std::setw(2) << unsigned(frame.data[i]);
  std::cout << std::dec << std::nouppercase << std::setfill(' ') << '\n' << std::flush;
}

bool Wait(int fd, uint32_t timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!stopped) {
    int wait_ms = 200;
    if (timeout_ms != 0) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (remaining <= 0) return false;
      wait_ms = static_cast<int>(std::min<int64_t>(remaining, wait_ms));
    }
    pollfd descriptor{fd, POLLIN, 0};
    const int result = poll(fd < 0 ? nullptr : &descriptor, fd < 0 ? 0 : 1, wait_ms);
    if (result < 0) {
      if (errno == EINTR) continue;
      SystemError("poll");
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
      throw std::runtime_error("CAN socket error or interface disconnected; inspect ip -details -statistics link");
    if (descriptor.revents & POLLIN) return true;
  }
  return false;
}

void RunTransmit(int fd, const Options& options) {
  uint64_t sent = 0;
  while (!stopped && (options.count == 0 || sent < options.count)) {
    const ssize_t bytes = write(fd, &options.frame, sizeof(options.frame));
    if (bytes < 0) {
      if (errno == EINTR) continue;
      SystemError("CAN write");
    }
    if (bytes != CAN_MTU) throw std::runtime_error("incomplete CAN write");
    ++sent;
    PrintFrame("tx", options.frame);
    if (options.count == 0 || sent < options.count) (void)Wait(-1, options.interval_ms);
  }
  // Socket acceptance is not an acknowledgement from an external CAN node.
  std::cout << "tx_queued=" << sent << " interrupted=" << (stopped ? "yes" : "no") << '\n';
}

void RunReceive(int fd, const Options& options) {
  uint64_t received = 0;
  while (!stopped && (options.count == 0 || received < options.count)) {
    if (!Wait(fd, options.timeout_ms)) {
      if (stopped) break;
      throw std::runtime_error("CAN receive timeout; use --timeout-ms 0 to wait indefinitely");
    }
    can_frame frame{};
    const ssize_t bytes = read(fd, &frame, sizeof(frame));
    if (bytes < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      SystemError("CAN read");
    }
    if (bytes != CAN_MTU || frame.len > CAN_MAX_DLEN || (frame.can_id & CAN_ERR_FLAG))
      throw std::runtime_error("invalid classical CAN data/remote frame");
    ++received;
    PrintFrame("rx", frame);
  }
  std::cout << "rx_received=" << received << " interrupted=" << (stopped ? "yes" : "no") << '\n';
}

void Help() {
  std::cout << "Usage: can_demo [options]\n"
      "Classical CAN via SocketCAN (TCAN4550 kernel driver required). No interface configuration.\n"
      "  --interface <name>   CAN device, default can0\n"
      "  --mode <rx|tx>       Default rx; tx sends one frame unless --count is set\n"
      "  --id <hex>           TX ID, default 123 (11-bit); optional 0x prefix\n"
      "  --extended           TX 29-bit extended ID (otherwise 11-bit)\n"
      "  --data <hexbytes>    TX 0..8 bytes, default 0102030405060708; no separators\n"
      "  --count <n>          Frame count; 0 runs until Ctrl-C; RX default 0, TX default 1\n"
      "  --interval-ms <n>    TX interval 1..3600000ms, default 1000\n"
      "  --timeout-ms <n>     RX idle timeout 0..3600000ms, default 0 (wait forever)\n"
      "  --help | --version   No CAN hardware access\n"
      "RX accepts all classical data/remote IDs, not CAN FD or error frames.\n"
      "TX success means queued to the kernel, not peer receipt. Local SocketCAN loopback\n"
      "may be seen by other local receivers; it is not an external-bus test.\n";
}
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "can_demo " << ROBOBATON_RELEASE_VERSION << '\n';
      return 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      Help();
      return 0;
    }
    const Options options = ParseOptions(argc, argv);
    struct sigaction action{};
    action.sa_handler = Stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0)
      SystemError("sigaction");
    CanSocket socket(options.interface);
    std::cout << "interface=" << options.interface << " mode=" << (options.transmit ? "tx" : "rx") << '\n';
    if (options.transmit) RunTransmit(socket.get(), options);
    else RunReceive(socket.get(), options);
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << "can_demo: " << error.what() << " (see --help)\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "can_demo: " << error.what() << '\n';
    return 1;
  }
}
