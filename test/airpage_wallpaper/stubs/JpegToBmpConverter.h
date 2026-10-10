#pragma once
#include <HalStorage.h>
struct JpegToBmpConverter {
  static inline bool decodeResult = true, lastCrop = false;
  static bool jpegFileToBmpStream(HalFile&, HalFile& output, bool crop) {
    lastCrop = crop;
    return decodeResult && output.write("BM:converted", 12) == 12;
  }
};
