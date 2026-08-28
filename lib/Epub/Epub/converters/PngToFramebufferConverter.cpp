#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
  uint32_t lastYieldMs{0};
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is ~42 KB due to internal zlib decompression buffers.
// We heap-allocate it on demand rather than using a static instance, so this memory
// is only consumed while actually decoding/querying PNG images. This is critical on
// the ESP32-C3 where total RAM is ~320 KB.
constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024;                          // ~42 KB + overhead
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 16 * 1024;  // decoder + 16 KB headroom

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int packedRowBytes(const int srcWidth, const int bitsPerSample) {
  return (srcWidth * bitsPerSample + 7) / 8;
}

int requiredPngInternalBufferBytes(const int srcWidth, const int pixelType, const int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(const int pixelType, const int bitsPerSample) {
  // PNGdec exposes the IHDR channel depth in iBpp. It expands standard
  // truecolor/alpha PNGs to 8 bits per channel; packed 1/2/4-bit samples are
  // valid only for grayscale and indexed-color PNGs.
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return bitsPerSample == 8;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return bitsPerSample == 8;
    case PNG_PIXEL_GRAY_ALPHA:
      return bitsPerSample == 8;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
      return bitsPerSample == 8 || bitsPerSample == 1 || bitsPerSample == 2 || bitsPerSample == 4;
    default:
      return false;
  }
}

uint8_t readPackedSample(const uint8_t* pixels, const int x, const int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];
  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((pixels[bitOffset >> 3] >> shift) & mask);
}

uint8_t expandSampleToByte(const uint8_t sample, const int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

// Convert entire source line to grayscale with alpha blending to white background.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t* p = &palette[readPackedSample(pPixels, x, bitsPerSample) * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Downscaling may map multiple source rows to one output row; upscaling must
  // repeat each source row across its complete destination range.
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx->dstHeight > ctx->srcHeight) {
    endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->srcHeight;
  }
  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                    pDraw->iHasAlpha);

  // Render scaled row using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;
  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    DirectPixelWriter pw;
    pw.init(*ctx->renderer);
    pw.beginRow(outY);

    bool caching = ctx->caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx->cache.advanceTo(dstY)) {
        caching = false;
        ctx->caching = false;
      } else {
        cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
        cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
      }
    }

    int srcX = 0;
    int error = 0;
    for (int dstX = 0; dstX < dstWidth; dstX++) {
      int outX = outXBase + dstX;
      if (outX < screenWidth) {
        uint8_t gray = ctx->grayLineBuffer[srcX];

        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          ditheredGray = gray / 85;
          if (ditheredGray > 3) ditheredGray = 3;
        }
        pw.writePixel(outX, ditheredGray);
        if (caching) cw.writePixel(outX, ditheredGray);
      }

      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
  }

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder for dimensions");
    return false;
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};

  if (rc != 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %d", rc);
    return false;
  }

  return validateAndStoreDimensions(png->getWidth(), png->getHeight(), out, "PNG");
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function
  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(png->getWidth(), png->getHeight(), sourceDimensions, "PNG")) {
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    // A zero/negative bound means "no limit", matching the JPEG path and
    // keeping direct decoder callers from producing a zero-sized target.
    float scaleX = (config.maxWidth > 0 && ctx.srcWidth > config.maxWidth)
                       ? (float)config.maxWidth / ctx.srcWidth
                       : 1.0f;
    float scaleY = (config.maxHeight > 0 && ctx.srcHeight > config.maxHeight)
                       ? (float)config.maxHeight / ctx.srcHeight
                       : 1.0f;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }

  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) {
    LOG_ERR("PNG", "Degenerate output dimensions %dx%d for %s, skipping render", ctx.dstWidth, ctx.dstHeight,
            imagePath.c_str());
    return false;
  }
  ctx.lastDstY = -1;  // Reset row tracking

  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.scale, png->getBpp());

  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("PNG",
            "PNG row buffer too small: need %d bytes for width=%d type=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
            requiredInternal, ctx.srcWidth, pixelType, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    return false;
  }

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png->getBpp()) + "bpp)", imagePath);
    return false;
  }

  // PNGdec supplies packed rows for low-bit-depth images, but conversion
  // expands every source pixel to one byte before dithering. Allocate exactly
  // one expanded row and use a no-throw allocator so low memory is recoverable.
  constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = PNG_MAX_BUFFERED_PIXELS / 2;
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for width=%d, max=%u",
            static_cast<unsigned>(grayBufSize), ctx.srcWidth, static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    return false;
  }
  auto grayLineBuffer = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
  if (!grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer (%u bytes)", static_cast<unsigned>(grayBufSize));
    return false;
  }
  ctx.grayLineBuffer = grayLineBuffer.get();

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  ctx.grayLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
