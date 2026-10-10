#pragma once

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <Utf8.h>

#include <deque>
#include <string>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    const auto isSpace = [](uint32_t cp) {
      return cp == ' ' || cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
             cp == 0x205F || cp == 0x3000;
    };
    return left == 0 || right == 0 || isSpace(left) || isSpace(right) || utf8IsCombiningMark(left) ||
                   utf8IsCombiningMark(right)
               ? 0
               : tracking;
  }
  struct DrawCall {
    int x;
    std::string text;
    EpdFontFamily::Style style;
    int8_t tracking;
  };
  bool captureDraws = false;
  mutable std::vector<DrawCall> draws;
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void drawLine(int, int, int, int, bool) const {}
  void drawText(int, int x, int, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const {
    if (captureDraws) draws.push_back({x, text, style, tracking});
  }
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      const int advance = cp == ' ' ? 4 : 8;
      width += ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0 ? advance / 2 : advance) +
               trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
  using TextGetter = const char* (*)(const void*, uint32_t);
  void ensureSdCardFontReady(int, TextGetter, const void*, uint32_t, bool, uint8_t) const {}
  void ensureSdCardFontReady(int, const char*, uint8_t) const {}
};
