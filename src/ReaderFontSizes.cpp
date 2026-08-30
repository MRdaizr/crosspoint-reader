#include "ReaderFontSizes.h"

#include <iterator>

#include "CrossPointSettings.h"

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      auto sizes = family->availableSizes();
      if (!sizes.empty()) return sizes;
    }
  }
#ifdef OMIT_FONTS
  return {CrossPointSettings::DEFAULT_FONT_POINT_SIZE};
#else
  return {std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES)};
#endif
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pointSize) {
  if (!sizes || count == 0) return pointSize;
  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pointSize ? best - pointSize : pointSize - best;
  for (size_t i = 1; i < count; ++i) {
    const uint8_t delta = sizes[i] > pointSize ? sizes[i] - pointSize : pointSize - sizes[i];
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}
