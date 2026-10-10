#pragma once
#include <cstdint>
struct CrossPointSettings {
  enum SLEEP_SCREEN_MODE { CUSTOM = 1, TRANSPARENT_CUSTOM = 2 };
  uint8_t sleepScreen = 0;
  bool saveResult = true;
  bool saveToFile() const { return saveResult; }
};
inline CrossPointSettings SETTINGS;
