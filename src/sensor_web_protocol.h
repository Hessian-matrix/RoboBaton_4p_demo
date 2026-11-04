#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace robobaton_demo::sensor_web {
constexpr uint32_t kVideoMagic = 0x53575631U;
constexpr uint32_t kImuMagic = 0x53574932U;
constexpr uint32_t kControlMagic = 0x53574331U;
constexpr uint32_t kStatusMagic = 0x53575331U;
constexpr uint32_t kRecordingCommandMagic = 0x53575231U;
constexpr uint32_t kRecordingStatusMagic = 0x53575232U;
constexpr size_t kVideoHeader = 80U;
constexpr size_t kChunkBytes = 60U * 1024U;
constexpr size_t kMaxAuBytes = 2U * 1024U * 1024U;
constexpr uint64_t kMaxAgeNs = 250000000U;
inline void Put16(uint8_t* p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v >> 8U); p[1] = static_cast<uint8_t>(v);
}
inline void Put32(uint8_t* p, uint32_t v) noexcept {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24U - 8U*i));
}
inline void Put64(uint8_t* p, uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56U - 8U*i));
}
inline uint32_t Get32(const uint8_t* p) noexcept {
  uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v = (v << 8U) | p[i]; return v;
}
inline uint64_t Get64(const uint8_t* p) noexcept {
  uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v = (v << 8U) | p[i]; return v;
}
inline void PutDouble(uint8_t* p, double value) noexcept {
  static_assert(sizeof(double) == sizeof(uint64_t));
  uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); Put64(p, bits);
}
inline void Header(uint8_t* p, uint32_t magic, uint16_t bytes, uint64_t generation) noexcept {
  Put32(p, magic); Put16(p+4, 1); Put16(p+6, bytes); Put64(p+8, generation);
}
}  // namespace robobaton_demo::sensor_web
