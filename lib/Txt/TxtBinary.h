#pragma once
#include <cstddef>
#include <cstdint>

namespace TxtBinary {
inline uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
inline uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t(get32(p + 4)) << 32); }
inline void put64(uint8_t* p, uint64_t v) {
  put32(p, uint32_t(v));
  put32(p + 4, uint32_t(v >> 32));
}
constexpr uint64_t HASH_SEED = 14695981039346656037ULL;
inline uint64_t hash(const uint8_t* p, size_t n, uint64_t h = HASH_SEED) {
  while (n--) {
    h ^= *p++;
    h *= 1099511628211ULL;
  }
  return h;
}
inline uint32_t checksum(const uint8_t* p, size_t n) { return uint32_t(hash(p, n)); }
}  // namespace TxtBinary
