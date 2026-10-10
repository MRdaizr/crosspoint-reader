#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
// Image decoding is outside these text/link fixtures. Keep Page's real dispatch
// linked, without bringing hardware codecs or display buffers into the host.
ImageBlock::ImageBlock(const std::string& path, const std::string& src, int16_t w, int16_t h)
    : imagePath(path), srcPath(src), width(w), height(h) {}
void ImageBlock::render(GfxRenderer&, int, int) {}
bool ImageBlock::serialize(HalFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t w, int64_t h, ImageDimensions& out, const char*) {
  if (w <= 0 || h <= 0 || w > INT16_MAX || h > INT16_MAX) return false;
  out = {static_cast<int16_t>(w), static_cast<int16_t>(h)};
  return true;
}
