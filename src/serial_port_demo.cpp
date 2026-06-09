#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_should_stop{false};

enum class RunMode {
  Tx,
  Rx,
  TxRx,
  Echo,
};

struct Options {
  std::string port = "/dev/ttyS1";
  int baud_rate = 115200;
  RunMode mode = RunMode::TxRx;
  uint32_t count = 0;
  uint32_t interval_ms = 1000;
  uint32_t timeout_ms = 200;
  std::string text = "uart-demo";
  bool append_newline = true;
};

[[noreturn]] void ThrowErrno(const std::string& message) {
  std::ostringstream oss;
  oss << message << ", errno=" << errno << " (" << strerror(errno) << ")";
  throw std::runtime_error(oss.str());
}

void SignalHandler(int /*signum*/) { g_should_stop.store(true); }

std::string RequireValue(int argc, char** argv, int* index, const char* name) {
  if (*index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + name);
  }
  ++(*index);
  return std::string(argv[*index]);
}

std::string NormalizePortPath(const std::string& port) {
  if (port.rfind("/dev/", 0) == 0) {
    return port;
  }
  if (port.rfind("tty", 0) == 0 || port.rfind("TTY", 0) == 0) {
    return "/dev/" + port;
  }
  return port;
}

RunMode ParseMode(const std::string& mode) {
  if (mode == "tx") {
    return RunMode::Tx;
  }
  if (mode == "rx") {
    return RunMode::Rx;
  }
  if (mode == "txrx") {
    return RunMode::TxRx;
  }
  if (mode == "echo") {
    return RunMode::Echo;
  }
  throw std::invalid_argument("--mode must be tx, rx, txrx, or echo");
}

const char* ModeName(RunMode mode) {
  switch (mode) {
    case RunMode::Tx:
      return "tx";
    case RunMode::Rx:
      return "rx";
    case RunMode::TxRx:
      return "txrx";
    case RunMode::Echo:
      return "echo";
  }
  return "unknown";
}

speed_t ToSpeedFlag(int baud_rate) {
  switch (baud_rate) {
    case 9600:
      return B9600;
    case 19200:
      return B19200;
    case 38400:
      return B38400;
    case 57600:
      return B57600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    case 460800:
      return B460800;
    case 921600:
      return B921600;
    default:
      throw std::invalid_argument("unsupported baud rate");
  }
}

Options ParseCommandLine(int argc, char** argv) {
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--port") {
      options.port = NormalizePortPath(RequireValue(argc, argv, &i, "--port"));
    } else if (arg == "--baud") {
      options.baud_rate = std::stoi(RequireValue(argc, argv, &i, "--baud"));
    } else if (arg == "--mode") {
      options.mode = ParseMode(RequireValue(argc, argv, &i, "--mode"));
    } else if (arg == "--count") {
      options.count = static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &i, "--count")));
    } else if (arg == "--interval-ms") {
      options.interval_ms =
          static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &i, "--interval-ms")));
    } else if (arg == "--timeout-ms") {
      options.timeout_ms =
          static_cast<uint32_t>(std::stoul(RequireValue(argc, argv, &i, "--timeout-ms")));
    } else if (arg == "--text") {
      options.text = RequireValue(argc, argv, &i, "--text");
    } else if (arg == "--no-newline") {
      options.append_newline = false;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: serial_port_demo [options]\n"
                << "  --port <path>             Serial device, default /dev/ttyS1\n"
                << "  --baud <rate>             Baud rate, default 115200\n"
                << "  --mode <tx|rx|txrx|echo>  Run mode, default txrx\n"
                << "  --count <n>               tx/txrx rounds or rx/echo packets, 0 means forever\n"
                << "  --interval-ms <ms>        TX interval, default 1000\n"
                << "  --timeout-ms <ms>         RX timeout, default 200\n"
                << "  --text <str>              TX payload prefix, default uart-demo\n"
                << "  --no-newline              Do not append newline to TX payload\n"
                << "  -h, --help                Show this help\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }

  return options;
}

uint64_t SteadyClockNowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

class SerialPort {
 public:
  SerialPort(const std::string& port, int baud_rate) : port_(port) {
    fd_ = open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
      ThrowErrno("failed to open " + port_);
    }
    Configure(baud_rate);
  }

  ~SerialPort() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  void WriteAll(const std::vector<uint8_t>& data) const {
    size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t written = write(fd_, data.data() + offset, data.size() - offset);
      if (written < 0) {
        ThrowErrno("failed to write " + port_);
      }
      offset += static_cast<size_t>(written);
    }
    if (tcdrain(fd_) != 0) {
      ThrowErrno("failed to drain " + port_);
    }
  }

  std::vector<uint8_t> ReadSome(size_t max_bytes, int timeout_ms) const {
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;

    const int poll_result = poll(&descriptor, 1, timeout_ms);
    if (poll_result < 0) {
      ThrowErrno("failed to poll " + port_);
    }
    if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) {
      return {};
    }

    std::vector<uint8_t> data(max_bytes, 0);
    const ssize_t bytes = read(fd_, data.data(), data.size());
    if (bytes < 0) {
      ThrowErrno("failed to read " + port_);
    }
    data.resize(static_cast<size_t>(bytes));
    return data;
  }

 private:
  void Configure(int baud_rate) {
    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
      ThrowErrno("failed to get termios for " + port_);
    }

    // 串口使用 8N1 raw 模式并关闭硬件流控，便于直接验证原始字节链路。
    cfmakeraw(&tty);
    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    tty.c_cflag &= static_cast<tcflag_t>(~PARENB);
    tty.c_cflag &= static_cast<tcflag_t>(~CSTOPB);
    tty.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
    tty.c_cflag &= static_cast<tcflag_t>(~CSIZE);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    const speed_t speed = ToSpeedFlag(baud_rate);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
      ThrowErrno("failed to set termios for " + port_);
    }
  }

  std::string port_;
  int fd_ = -1;
};

std::vector<uint8_t> BuildPayload(const Options& options, uint32_t sequence) {
  std::ostringstream oss;
  oss << options.text << " seq=" << sequence;
  if (options.append_newline) {
    oss << '\n';
  }

  const std::string text = oss.str();
  return std::vector<uint8_t>(text.begin(), text.end());
}

std::string ToHex(const std::vector<uint8_t>& data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < data.size(); ++i) {
    oss << std::setw(2) << static_cast<int>(data[i]);
    if (i + 1 < data.size()) {
      oss << ' ';
    }
  }
  return oss.str();
}

std::string ToAscii(const std::vector<uint8_t>& data) {
  std::string text;
  text.reserve(data.size());
  for (uint8_t value : data) {
    text.push_back(std::isprint(value) ? static_cast<char>(value) : '.');
  }
  return text;
}

void PrintFrame(const char* tag, uint32_t sequence, const std::vector<uint8_t>& data) {
  std::cout << "ts_ns=" << SteadyClockNowNs()
            << " " << tag << "_seq=" << sequence
            << " " << tag << "_bytes=" << data.size()
            << " " << tag << "_hex=[" << ToHex(data) << "]"
            << " " << tag << "_ascii=[" << ToAscii(data) << "]\n";
}

bool ReachedCount(uint32_t count, uint32_t limit) { return limit != 0 && count >= limit; }

void RunTx(const Options& options, const SerialPort& port) {
  for (uint32_t seq = 0; !g_should_stop.load() && !ReachedCount(seq, options.count); ++seq) {
    const std::vector<uint8_t> payload = BuildPayload(options, seq);
    port.WriteAll(payload);
    PrintFrame("tx", seq, payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
  }
}

void RunRx(const Options& options, const SerialPort& port, bool echo) {
  uint32_t rx_seq = 0;
  while (!g_should_stop.load() && !ReachedCount(rx_seq, options.count)) {
    const std::vector<uint8_t> data = port.ReadSome(256, static_cast<int>(options.timeout_ms));
    if (data.empty()) {
      continue;
    }
    PrintFrame(echo ? "echo_rx" : "rx", rx_seq, data);
    if (echo) {
      port.WriteAll(data);
      PrintFrame("echo_tx", rx_seq, data);
    }
    ++rx_seq;
  }
}

void RunTxRx(const Options& options, const SerialPort& port) {
  uint32_t rx_seq = 0;
  for (uint32_t tx_seq = 0; !g_should_stop.load() && !ReachedCount(tx_seq, options.count);
       ++tx_seq) {
    const std::vector<uint8_t> payload = BuildPayload(options, tx_seq);
    port.WriteAll(payload);
    PrintFrame("tx", tx_seq, payload);

    const std::vector<uint8_t> data = port.ReadSome(256, static_cast<int>(options.timeout_ms));
    if (!data.empty()) {
      PrintFrame("rx", rx_seq++, data);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    const Options options = ParseCommandLine(argc, argv);
    std::cout << "UART demo: port=" << options.port << " baud=" << options.baud_rate
              << " mode=" << ModeName(options.mode) << "\n";

    SerialPort port(options.port, options.baud_rate);
    switch (options.mode) {
      case RunMode::Tx:
        RunTx(options, port);
        break;
      case RunMode::Rx:
        RunRx(options, port, false);
        break;
      case RunMode::TxRx:
        RunTxRx(options, port);
        break;
      case RunMode::Echo:
        RunRx(options, port, true);
        break;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
