#pragma once

#include <Print.h>

#include <cstdint>
#include <string_view>

// Markdown deliberately follows the plain-text path, including its literal symbols.
class TxtToHtml {
 public:
  struct Callbacks {
    void* context = nullptr;
    bool (*cancelled)(void*) = nullptr;
    void (*progress)(void*, uint32_t sourceBytes, uint32_t totalBytes) = nullptr;
  };
  // Checkpoints are taken only between complete UTF-8 codepoints. Deferred spaces
  // must survive a checkpoint: they may subsequently be emitted OR discarded.
  struct State {
    uint32_t source = 0;
    uint32_t visible = 1;  // The newline immediately after <body> is character data.
    uint32_t spaces = 0;
    uint32_t spaceStart = 0;
    bool lineStart = true;
  };
  struct Event {
    uint32_t sourceStart, sourceEnd, visibleStart, count;
    bool spaces;
  };
  struct Observer {
    void* context = nullptr;
    // false stops replay successfully (the desired position has been found).
    bool (*event)(void*, const Event&) = nullptr;
    bool (*checkpoint)(void*, const State&) = nullptr;
  };
  using Read = int (*)(void*, uint8_t*, size_t);
  static const char* cacheVersionTag(std::string_view filename);
  static bool stream(std::string_view filename, void* context, Read read, Print& out);
  static bool stream(std::string_view filename, std::string_view content, Print& out);
  static bool convert(std::string_view filename, void* context, Read read, Print& out, uint32_t total,
                      const Callbacks* callbacks, const Observer* observer, State& state, bool replay = false);
};
