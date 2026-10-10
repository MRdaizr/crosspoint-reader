#pragma once

#include <cstddef>
#include <cstdint>

namespace TxtResumeOffset {
// A small prefix ending at/beyond the requested byte is enough to avoid a
// UTF-8 continuation byte. Unlike percentage jumps, resume never scans forward.
inline size_t characterStart(const uint8_t* prefix, size_t length, size_t relative) {
  if (!prefix || relative >= length) return relative;
  while (relative > 0 && (prefix[relative] & 0xC0) == 0x80) --relative;
  return relative;
}
}  // namespace TxtResumeOffset
