#pragma once
struct LinkTestSettings {
  int getReaderFontId() const { return 0; }
};
inline LinkTestSettings linkTestSettings;
#define SETTINGS linkTestSettings
