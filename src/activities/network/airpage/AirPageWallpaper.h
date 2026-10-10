#pragma once

#include "AirPageImageStore.h"

namespace airpage {

class AirPageWallpaper final {
 public:
  static void recoverInterruptedTransaction();
  static bool install(const SelectedImage& selected);
  // Shared with the file viewer; accepts full SD paths, not the bounded AirPage history path.
  static bool installFile(const char* path, ImageFormat format, bool crop = false);
  static bool installOverlay(const char* path, const char* destination);

 private:
  static bool writePart(const char* path, ImageFormat format, bool crop);
  static bool copyFile(const char* sourcePath, const char* targetPath);
};

}  // namespace airpage
