#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Built-in reader faces are generated at these physical point sizes.  SD card
// families may expose a different set; the text-settings screen presents the
// sizes that are actually installed instead of pretending every family has the
// old Small/Medium/Large slots.
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12, 14, 16, 18};

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName);

// Return the nearest available point size.  A tie intentionally chooses the
// smaller size, making a setting deterministic when a family has e.g. 13/15pt.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pointSize);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, uint8_t pointSize) {
  return sizes.empty() ? pointSize : snapToNearestPointSize(sizes.data(), sizes.size(), pointSize);
}
