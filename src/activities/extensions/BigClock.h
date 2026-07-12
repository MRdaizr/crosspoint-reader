#pragma once

#include <GfxRenderer.h>

namespace BigClock {
constexpr int DIGIT_WIDTH = 86;
constexpr int DIGIT_HEIGHT = 190;
constexpr int DIGIT_STROKE = 15;
constexpr int DIGIT_GAP = 12;
constexpr int COLON_WIDTH = 28;

inline int top(const int pageHeight) { return pageHeight / 2 - DIGIT_HEIGHT / 2 - 35; }

inline void drawDigit(const GfxRenderer& renderer, const int x, const int y, const int digit) {
  static constexpr uint8_t SEGMENTS[] = {
      0b00111111, 0b00000110, 0b01011011, 0b01001111, 0b01100110,
      0b01101101, 0b01111101, 0b00000111, 0b01111111, 0b01101111,
  };
  const uint8_t segments = SEGMENTS[digit % 10];
  const int halfHeight = (DIGIT_HEIGHT - DIGIT_STROKE) / 2;
  const int horizontalWidth = DIGIT_WIDTH - 2 * DIGIT_STROKE;
  const auto draw = [&](const uint8_t segment, const int sx, const int sy, const int width, const int height) {
    if (segments & (1 << segment)) renderer.fillRect(sx, sy, width, height);
  };
  draw(0, x + DIGIT_STROKE, y, horizontalWidth, DIGIT_STROKE);
  draw(1, x + DIGIT_WIDTH - DIGIT_STROKE, y + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(2, x + DIGIT_WIDTH - DIGIT_STROKE, y + halfHeight + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(3, x + DIGIT_STROKE, y + DIGIT_HEIGHT - DIGIT_STROKE, horizontalWidth, DIGIT_STROKE);
  draw(4, x, y + halfHeight + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(5, x, y + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(6, x + DIGIT_STROKE, y + halfHeight, horizontalWidth, DIGIT_STROKE);
}

inline void drawTime(const GfxRenderer& renderer, const int pageWidth, const int pageHeight, const int hour,
                     const int minute) {
  const int width = DIGIT_WIDTH * 4 + DIGIT_GAP * 4 + COLON_WIDTH;
  const int x = (pageWidth - width) / 2;
  const int y = top(pageHeight);
  drawDigit(renderer, x, y, hour / 10);
  drawDigit(renderer, x + DIGIT_WIDTH + DIGIT_GAP, y, hour % 10);
  constexpr int DOT = 18;
  const int colonX = x + (DIGIT_WIDTH + DIGIT_GAP) * 2;
  renderer.fillRect(colonX + (COLON_WIDTH - DOT) / 2, y + DIGIT_HEIGHT / 3 - DOT / 2, DOT, DOT);
  renderer.fillRect(colonX + (COLON_WIDTH - DOT) / 2, y + DIGIT_HEIGHT * 2 / 3 - DOT / 2, DOT, DOT);
  const int minuteX = colonX + COLON_WIDTH + DIGIT_GAP;
  drawDigit(renderer, minuteX, y, minute / 10);
  drawDigit(renderer, minuteX + DIGIT_WIDTH + DIGIT_GAP, y, minute % 10);
}
}  // namespace BigClock
