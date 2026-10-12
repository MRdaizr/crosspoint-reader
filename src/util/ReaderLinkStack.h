#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace readerlinks {

struct Position {
  int spineIndex;
  int pageNumber;
};

inline constexpr int MAX_DEPTH = 3;
inline constexpr size_t MAX_BYTES = 1 + MAX_DEPTH * 4;

inline bool validPosition(const Position& position, const int spineCount = 65536) {
  return position.spineIndex >= 0 && position.spineIndex < spineCount && position.spineIndex <= UINT16_MAX &&
         position.pageNumber >= 0 && position.pageNumber < UINT16_MAX;
}

inline bool push(Position* positions, int& depth, const Position origin) {
  if (depth < 0 || depth > MAX_DEPTH || !validPosition(origin)) return false;
  if (depth == MAX_DEPTH) {
    std::copy(positions + 1, positions + MAX_DEPTH, positions);
    --depth;
  }
  positions[depth++] = origin;
  return true;
}

// links.bin: depth, then depth pairs of little-endian u16 spine/page.
inline size_t encode(const Position* positions, const int depth, uint8_t (&data)[MAX_BYTES]) {
  if (depth <= 0 || depth > MAX_DEPTH) return 0;
  for (int i = 0; i < depth; ++i) {
    if (!validPosition(positions[i])) return 0;
  }
  data[0] = static_cast<uint8_t>(depth);
  for (int i = 0; i < depth; ++i) {
    const size_t offset = 1 + i * 4;
    data[offset] = positions[i].spineIndex & 0xFF;
    data[offset + 1] = (positions[i].spineIndex >> 8) & 0xFF;
    data[offset + 2] = positions[i].pageNumber & 0xFF;
    data[offset + 3] = (positions[i].pageNumber >> 8) & 0xFF;
  }
  return 1 + depth * 4;
}

inline Position readPosition(const uint8_t* data) { return {data[0] | (data[1] << 8), data[2] | (data[3] << 8)}; }

inline int decode(const uint8_t* data, const size_t size, const int spineCount, Position* positions) {
  if (size == 0 || size > MAX_BYTES) return 0;
  const int depth = data[0];
  if (depth <= 0 || depth > MAX_DEPTH || size != 1 + static_cast<size_t>(depth) * 4) return 0;
  for (int i = 0; i < depth; ++i) {
    if (!validPosition(readPosition(data + 1 + i * 4), spineCount)) return 0;
  }
  for (int i = 0; i < depth; ++i) positions[i] = readPosition(data + 1 + i * 4);
  return depth;
}

}  // namespace readerlinks
