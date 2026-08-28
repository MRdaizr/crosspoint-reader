#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;

  // Clear the bounded per-session failure table when a new EPUB is opened.
  // A failed decode should render a placeholder once, not retry on every
  // grayscale pass or page refresh until the reader is restarted.
  static void clearSessionRenderFailures();
  static void releaseRenderCache();

  // The section builder stores only image dimensions and the book-internal href.
  // The actual image is extracted into imagePath on first render, keeping
  // image-heavy chapter builds within the X4 heap budget.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool renderChecked(GfxRenderer& renderer, int x, int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;
  int16_t width;
  int16_t height;

  static void* extractCtx;
  static ExtractFn extractFn;
};
