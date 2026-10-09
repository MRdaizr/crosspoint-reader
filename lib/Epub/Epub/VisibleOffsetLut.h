#pragma once
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <optional>

// The caller validates the on-disk watermark and seeks to its visible LUT.
// Reading at most 32 starts per call cuts SD overhead without a heap buffer.
template <typename File>
std::optional<uint16_t> pageForVisibleOffset(File& file, uint16_t count, uint32_t offset, bool preferFirst,
                                             bool partial) {
  if (count == 0) return std::nullopt;
  uint16_t result = 0;
  uint32_t lastOffset = 0;
  constexpr uint32_t BATCH_SIZE = 32;
  uint32_t starts[BATCH_SIZE];
  for (uint32_t base = 0; base < count; base += BATCH_SIZE) {
    const uint32_t batch = std::min<uint32_t>(BATCH_SIZE, count - base);
    const size_t bytes = batch * sizeof(uint32_t);
    if (file.read(reinterpret_cast<uint8_t*>(starts), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SCT", "Failed to read visible offset LUT");
      return std::nullopt;
    }
    for (uint32_t i = 0; i < batch; ++i) {
      lastOffset = starts[i];
      if (lastOffset > offset) return result;
      result = static_cast<uint16_t>(base + i);
      if (preferFirst && lastOffset == offset) return result;
    }
  }
  // A partial cache is only authoritative through the last recorded start.
  if (partial && offset > lastOffset) return std::nullopt;
  return result;
}
