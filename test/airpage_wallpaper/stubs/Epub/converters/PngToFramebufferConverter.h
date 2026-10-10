#pragma once
#include <HalStorage.h>
struct ImageDimensions {
  int width = 0, height = 0;
};
struct PngToFramebufferConverter {
  bool getDimensions(const char* path, ImageDimensions& out) const {
    if (!Storage.exists(path) || !wallpaperfake::files[path].starts_with("PNG:")) return false;
    out.width = out.height = 1;
    return true;
  }
};
