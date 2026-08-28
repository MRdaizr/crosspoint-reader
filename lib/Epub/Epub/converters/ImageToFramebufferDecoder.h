#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Decode callbacks are invoked from the parser task. Keep long image
  // decodes from starving the idle task and its watchdog.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them to int16_t.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // The decoders stream rows/MCUs, so the cap bounds decode time rather than
  // allocating a buffer proportional to the source image area.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
