#include "ImageDecoderFactory.h"

#include <Logging.h>

#include <cctype>
#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

namespace {
std::string extensionForPath(const std::string& imagePath) {
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos == std::string::npos) {
    return {};
  }

  std::string extension = imagePath.substr(dotPos);
  for (auto& c : extension) {
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  return extension;
}
}  // namespace

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  const std::string ext = extensionForPath(imagePath);

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new JpegToFramebufferConverter());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new PngToFramebufferConverter());
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  const std::string ext = extensionForPath(imagePath);
  return JpegToFramebufferConverter::supportsFormat(ext) || PngToFramebufferConverter::supportsFormat(ext);
}
