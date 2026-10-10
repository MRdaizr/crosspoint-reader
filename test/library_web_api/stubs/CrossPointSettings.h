#pragma once
#include <cstdint>
class CrossPointSettings {
 public:
  uint8_t libraryUseMetadata = 1;
  static CrossPointSettings& getInstance() {
    static CrossPointSettings settings;
    return settings;
  }
};
#define SETTINGS CrossPointSettings::getInstance()
