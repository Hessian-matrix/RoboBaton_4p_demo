#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace robobaton_demo {

// Read-only HTTP snapshots for a trusted LAN; this server has no authentication.
class PanoramaHttpServer final {
 public:
  PanoramaHttpServer();
  ~PanoramaHttpServer();
  PanoramaHttpServer(const PanoramaHttpServer&) = delete;
  PanoramaHttpServer& operator=(const PanoramaHttpServer&) = delete;

  // Start/Stop belong to one lifecycle owner. Start throws on invalid IPv4,
  // socket/bind/listen failure, or an already-started server; it never shares a port.
  void Start(const std::string& numeric_ipv4, uint16_t port);

  // May run concurrently with HTTP and Stop. The JPEG must remain immutable.
  // Replaces, rather than queues, the latest frame and caller-supplied valid JSON.
  // A null/empty JPEG makes /frame.jpg unavailable without hiding status_json.
  void Publish(std::shared_ptr<const std::vector<uint8_t>> jpeg,
               std::string status_json);
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace robobaton_demo
