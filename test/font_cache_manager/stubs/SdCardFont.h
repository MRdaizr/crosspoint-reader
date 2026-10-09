#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

class SdCardFont {
 public:
  struct PrewarmCall {
    char text[32] = {};
    uint8_t styleMask = 0;
  };

  void clearCache() {}
  unsigned releaseCount = 0;
  void releaseResidentCaches() { ++releaseCount; }
  using TextGetter = const char* (*)(const void*, uint32_t);
  int prewarm(const char* text, uint8_t styleMask, bool = false, bool = true, bool = true) {
    auto& call = prewarmCalls[prewarmCallCount++];
    std::snprintf(call.text, sizeof(call.text), "%s", text);
    call.styleMask = styleMask;
    return 0;
  }
  uint8_t resolveStyle(uint8_t style) const { return resolvedStyles[style & 0x03]; }
  int prewarm(TextGetter getter, const void* ctx, uint32_t count, uint8_t styleMask, bool = false, bool = true,
              bool = true) {
    char text[32] = {};
    for (uint32_t i = 0; i < count; ++i) {
      const char* item = getter(ctx, i);
      std::strncat(text, item, sizeof(text) - std::strlen(text) - 1);
    }
    return prewarm(text, styleMask);
  }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[4] = {};
  int prewarmCallCount = 0;
  uint8_t resolvedStyles[4] = {0, 1, 2, 3};
};
