#pragma once
// Reuse the spacing metric fixture unchanged; add only UI snapshot instrumentation.
#define GfxRenderer LinkMetricsRenderer
#include "../../parsed_text/stubs/GfxRenderer.h"
#undef GfxRenderer
#include <array>
#include <cstring>

struct HalDisplay {
  enum RefreshMode { FAST_REFRESH };
};
class GfxRenderer : public LinkMetricsRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  struct Rect {
    int x, y, w, h;
  };
  Orientation orientation = Portrait;
  mutable std::array<uint8_t, 48000> framebuffer{};
  mutable int clears = 0, captures = 0, restores = 0, displays = 0;
  mutable size_t largestCapture = 0;
  bool failCapture = false, failRestore = false;
  mutable std::vector<Rect> rectangles;
  int getScreenWidth() const { return orientation == Portrait || orientation == PortraitInverted ? 480 : 800; }
  int getScreenHeight() const { return orientation == Portrait || orientation == PortraitInverted ? 800 : 480; }
  Orientation getOrientation() const { return orientation; }
  void setOrientation(Orientation o) { orientation = o; }
  void clearScreen() const {
    ++clears;
    framebuffer.fill(255);
  }
  void displayBuffer(HalDisplay::RefreshMode) const { ++displays; }
  void drawRect(int x, int y, int w, int h, int, bool) const { rectangles.push_back({x, y, w, h}); }
  bool bounds(int x, int y, int w, int h, int& x1, int& y1, int& x2, int& y2) const {
    if (w <= 0 || h <= 0) return false;
    x1 = std::max(0, x);
    y1 = std::max(0, y);
    x2 = std::min(getScreenWidth() - 1, x + w - 1);
    y2 = std::min(getScreenHeight() - 1, y + h - 1);
    if (x2 < x1 || y2 < y1) return false;
    int a = x1, b = y1, c = x2, d = y2;
    if (orientation == Portrait) {
      x1 = b;
      x2 = d;
      y1 = 479 - c;
      y2 = 479 - a;
    } else if (orientation == PortraitInverted) {
      x1 = 799 - d;
      x2 = 799 - b;
      y1 = a;
      y2 = c;
    } else if (orientation == LandscapeClockwise) {
      x1 = 799 - c;
      x2 = 799 - a;
      y1 = 479 - d;
      y2 = 479 - b;
    }
    return true;
  }
  size_t getRegionByteSize(int x, int y, int w, int h) const {
    int x1, y1, x2, y2;
    return bounds(x, y, w, h, x1, y1, x2, y2) ? static_cast<size_t>(x2 / 8 - x1 / 8 + 1) * (y2 - y1 + 1) : 0;
  }
  bool copyRegionToBuffer(int x, int y, int w, int h, uint8_t* buf, size_t capacity) const {
    const size_t bytes = getRegionByteSize(x, y, w, h);
    if (failCapture || !buf || !bytes || bytes > capacity) return false;
    largestCapture = std::max(largestCapture, bytes);
    ++captures;
    std::memset(buf, 255, bytes);
    return true;
  }
  bool copyBufferToRegion(int x, int y, int w, int h, const uint8_t* buf, size_t capacity) const {
    const size_t bytes = getRegionByteSize(x, y, w, h);
    if (failRestore || !buf || !bytes || bytes > capacity) return false;
    ++restores;
    return true;
  }
};
