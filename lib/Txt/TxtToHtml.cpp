#include "TxtToHtml.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <limits>

#include "TxtIo.h"

namespace {
constexpr size_t BUFFER_SIZE = 1024;
class Output {
  Print& out;
  std::unique_ptr<uint8_t[]> buffer;
  size_t used = 0;

 public:
  explicit Output(Print& out) : out(out), buffer(makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE)) {}
  bool valid() const { return buffer != nullptr; }
  bool flush() {
    if (used && out.write(buffer.get(), used) != used) return false;
    used = 0;
    return true;
  }
  bool bytes(const char* s, size_t n) {
    while (n--) {
      buffer[used++] = static_cast<uint8_t>(*s++);
      if (used == BUFFER_SIZE && !flush()) return false;
    }
    return true;
  }
  bool text(const char* s) { return bytes(s, strlen(s)); }
  bool escaped(const char* s, size_t n) {
    if (n != 1) return bytes(s, n);
    switch (*s) {
      case '&':
        return text("&amp;");
      case '<':
        return text("&lt;");
      case '>':
        return text("&gt;");
      default:
        return bytes(s, n);
    }
  }
};
class Input {
  void* context;
  TxtToHtml::Read read;
  std::unique_ptr<uint8_t[]> buffer;
  size_t used = 0, length = 0;

 public:
  bool failed = false;
  Input(void* context, TxtToHtml::Read read)
      : context(context), read(read), buffer(makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE)) {}
  bool valid() const { return buffer != nullptr && read; }
  int byte() {
    if (used == length) {
      const int n = read(context, buffer.get(), BUFFER_SIZE);
      if (n < 0 || n > static_cast<int>(BUFFER_SIZE)) {
        failed = true;
        return -1;
      }
      if (!n) return -1;
      length = n;
      used = 0;
    }
    return buffer[used++];
  }
};
bool emit(const TxtToHtml::Observer* observer, const TxtToHtml::Event& event) {
  return !observer || !observer->event || observer->event(observer->context, event);
}
struct StringInput {
  std::string_view text;
  size_t offset = 0;
};
int readString(void* context, uint8_t* buffer, size_t size) {
  auto& input = *static_cast<StringInput*>(context);
  size = std::min(size, input.text.size() - input.offset);
  if (!size) return 0;
  memcpy(buffer, input.text.data() + input.offset, size);
  input.offset += size;
  return static_cast<int>(size);
}
}  // namespace

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
  const auto dot = filename.find_last_of('.');
  auto ext = dot == std::string_view::npos ? std::string_view{} : filename.substr(dot);
  return ext == ".md" || ext == ".MD" || ext == ".Md" || ext == ".mD" ? "<!-- MD_CACHE_VERSION: 1 -->"
                                                                      : "<!-- TXT_CACHE_VERSION: 1 -->";
}
bool TxtToHtml::stream(std::string_view filename, void* context, Read read, Print& out) {
  State state;
  return convert(filename, context, read, out, 0, nullptr, nullptr, state);
}
bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out) {
  StringInput input{content};
  return stream(filename, &input, readString, out);
}
bool TxtToHtml::convert(std::string_view filename, void* context, Read read, Print& out, uint32_t total,
                        const Callbacks* callbacks, const Observer* observer, State& state, bool replay) {
  // Two reusable 1KB buffers avoid book-sized allocations and the task stack.
  Input input(context, read);
  Output output(out);
  if (!input.valid() || !output.valid()) {
    LOG_ERR("TXT", "OOM converting text");
    return false;
  }
  const auto cancelled = [&] { return callbacks && callbacks->cancelled && callbacks->cancelled(callbacks->context); };
  if (cancelled()) return false;
  if (!replay) {
    const auto slash = filename.find_last_of("/\\");
    if (slash != std::string_view::npos) filename.remove_prefix(slash + 1);
    const auto dot = filename.find_last_of('.');
    const auto title = dot == std::string_view::npos ? filename : filename.substr(0, dot);
    if (!output.text("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n") || !output.text(cacheVersionTag(filename)) ||
        !output.text("\n<!DOCTYPE html>\n<html>\n<head><title>"))
      return false;
    for (char c : title)
      if (!output.escaped(&c, 1)) return false;
    if (!output.text("</title></head>\n<body>\n")) return false;
  }
  uint32_t lastProgress = state.source;
  for (;;) {
    if (cancelled()) return false;
    int byte = input.byte();
    if (byte < 0) break;
    const uint32_t start = state.source;
    char token[4] = {static_cast<char>(byte), 0, 0, 0};
    uint32_t cp = static_cast<uint32_t>(byte);
    unsigned count = 1;
    if (byte >= 0x80) {
      if (byte >= 0xC2 && byte <= 0xDF) {
        count = 2;
        cp = byte & 31;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        count = 3;
        cp = byte & 15;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        count = 4;
        cp = byte & 7;
      } else {
        LOG_ERR("TXT", "Invalid UTF8 at %u", start);
        return false;
      }
      for (unsigned i = 1; i < count; ++i) {
        const int tail = input.byte();
        if (tail < 0x80 || tail > 0xBF) return false;
        token[i] = static_cast<char>(tail);
        cp = (cp << 6) | (tail & 63);
      }
      if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000) || cp > 0x10FFFF ||
          (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE || cp == 0xFFFF)
        return false;
    }
    if (state.source > UINT32_MAX - count) return false;
    state.source += count;
    if (start == 0 && cp == 0xFEFF) { /* UTF8 BOM, even across short reads */
    } else if (cp == '\r') {          /* baseline ignores all CR */
    } else if (cp == '\n') {
      state.spaces = 0;
      state.lineStart = true;
      if (!output.text("<br />")) return false;
    } else if (cp == ' ' && !state.lineStart) {
      if (!state.spaces) state.spaceStart = start;
      if (state.spaces == UINT32_MAX) return false;
      ++state.spaces;
    } else {
      if (state.spaces) {
        if (state.visible > UINT32_MAX - state.spaces) return false;
        for (uint32_t i = 0; i < state.spaces; ++i) {
          if ((i % BUFFER_SIZE == 0) && !TxtIo::pulse(callbacks, state.source, total)) return false;
          if (!output.text(i + 1 == state.spaces ? " " : "&#160;")) return false;
        }
        const Event event{state.spaceStart, start, state.visible, state.spaces, true};
        state.visible += state.spaces;
        state.spaces = 0;
        if (!emit(observer, event)) return true;
      }
      if (state.visible == UINT32_MAX) return false;
      const Event event{start, state.source, state.visible++, 1, false};
      if (cp == ' ') {
        if (!output.text("&#160;")) return false;
      } else {
        if (cp < 0x20 && cp != '\t') token[0] = ' ';
        if (!output.escaped(token, count)) return false;
        state.lineStart = false;
      }
      if (!emit(observer, event)) return true;
    }
    if (observer && observer->checkpoint && !observer->checkpoint(observer->context, state)) return false;
    if (state.source - lastProgress >= BUFFER_SIZE) {
      lastProgress = state.source;
      if (!TxtIo::pulse(callbacks, state.source, total)) return false;
    }
  }
  if (input.failed || cancelled() || (total && state.source != total)) return false;
  state.spaces = 0;  // baseline discards trailing spaces
  if (state.visible == UINT32_MAX) return false;
  if (!emit(observer, {state.source, state.source, state.visible++, 1, false})) return true;
  if (!output.text("\n</body>\n</html>\n") || !output.flush()) return false;
  if (callbacks && callbacks->progress) callbacks->progress(callbacks->context, state.source, total);
  return true;
}
