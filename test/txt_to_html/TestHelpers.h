#pragma once
#include <Print.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <string>

class StringPrint : public Print {
 public:
  std::string text;
  bool fail = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (fail) return 0;
    text.append(reinterpret_cast<const char*>(p), n);
    return n;
  }
};
struct ShortReader {
  std::string text;
  size_t offset = 0, chunk = 1;
  static int read(void* context, uint8_t* p, size_t n) {
    auto& self = *static_cast<ShortReader*>(context);
    n = std::min({n, self.chunk, self.text.size() - self.offset});
    memcpy(p, self.text.data() + self.offset, n);
    self.offset += n;
    return int(n);
  }
};
// Generated documents contain only body text and <br/>. This Expat oracle uses
// the same counting boundary as ChapterHtmlSlimParser::characterData: all body
// codepoints, including whitespace/entity expansion, but NOT synthetic br text.
struct BodyOracle {
  bool body = false;
  uint32_t count = 0;
  std::string text;
  static void start(void* ctx, const char* name, const char**) {
    if (!strcmp(name, "body")) static_cast<BodyOracle*>(ctx)->body = true;
  }
  static void end(void* ctx, const char* name) {
    if (!strcmp(name, "body")) static_cast<BodyOracle*>(ctx)->body = false;
  }
  static void data(void* ctx, const char* text, int length) {
    auto& self = *static_cast<BodyOracle*>(ctx);
    if (!self.body) return;
    self.text.append(text, length);
    for (int i = 0; i < length; ++i)
      if ((uint8_t(text[i]) & 0xC0) != 0x80) ++self.count;
  }
  bool parse(const std::string& html) {
    auto parser = XML_ParserCreate(nullptr);
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, start, end);
    XML_SetCharacterDataHandler(parser, data);
    const bool ok = XML_Parse(parser, html.data(), int(html.size()), true) == XML_STATUS_OK;
    XML_ParserFree(parser);
    return ok;
  }
};
