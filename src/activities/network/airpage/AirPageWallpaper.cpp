#include "AirPageWallpaper.h"

#include <Epub/converters/PngToFramebufferConverter.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "CrossPointSettings.h"
#include "JpegToBmpConverter.h"

namespace airpage {

namespace {

constexpr char kSleepImagePath[] = "/sleep.bmp";
constexpr char kSleepImagePartPath[] = "/sleep.bmp.part";
constexpr char kSleepImageBackupPath[] = "/sleep.bmp.bak";
constexpr size_t kCopyBufferSize = 128;
constexpr char kOverlayBmp[] = "/sleep-overlay.bmp";
constexpr char kOverlayPng[] = "/sleep-overlay.png";

bool validOverlay(const char* path, const bool png) {
  if (!Storage.exists(path)) return false;
  if (png) {
    ImageDimensions dimensions;
    PngToFramebufferConverter decoder;
    return decoder.getDimensions(path, dimensions) && dimensions.width > 0 && dimensions.height > 0;
  }
  ImageInfo info;
  return AirPageImageStore::inspectImage(path, info) && info.format == ImageFormat::Bmp;
}

}  // namespace

bool AirPageWallpaper::copyFile(const char* sourcePath, const char* targetPath) {
  Storage.remove(targetPath);
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("AIRP", sourcePath, input) || !Storage.openFileForWrite("AIRP", targetPath, output)) {
    return false;
  }

  const uint64_t expected = input.fileSize64();
  uint64_t copied = 0;
  uint8_t buffer[kCopyBufferSize];
  while (input.available() > 0) {
    const size_t want = std::min<size_t>(sizeof(buffer), static_cast<size_t>(input.available()));
    const int bytesRead = input.read(buffer, want);
    if (bytesRead <= 0 || output.write(buffer, static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
      return false;
    }
    copied += static_cast<uint64_t>(bytesRead);
  }
  output.flush();
  return output.close() && copied == expected;
}

bool AirPageWallpaper::writePart(const char* path, const ImageFormat format, const bool crop) {
  Storage.remove(kSleepImagePartPath);
  switch (format) {
    case ImageFormat::None:
      return false;
    case ImageFormat::Bmp:
      return copyFile(path, kSleepImagePartPath);
    case ImageFormat::Jpeg: {
      HalFile input;
      HalFile output;
      if (!Storage.openFileForRead("AIRP", path, input) ||
          !Storage.openFileForWrite("AIRP", kSleepImagePartPath, output)) {
        return false;
      }
      const bool converted = JpegToBmpConverter::jpegFileToBmpStream(input, output, crop);
      output.flush();
      return output.close() && converted;
    }
  }
  return false;
}

bool AirPageWallpaper::install(const SelectedImage& selected) {
  return installFile(selected.path, selected.image.format);
}

bool AirPageWallpaper::installFile(const char* path, const ImageFormat format, const bool crop) {
  if (!path || !*path || format == ImageFormat::None) return false;
  recoverInterruptedTransaction();
  if (!writePart(path, format, crop)) {
    Storage.remove(kSleepImagePartPath);
    return false;
  }

  ImageInfo generated;
  if (!AirPageImageStore::inspectImage(kSleepImagePartPath, generated) || generated.format != ImageFormat::Bmp) {
    Storage.remove(kSleepImagePartPath);
    return false;
  }

  Storage.remove(kSleepImageBackupPath);
  const bool hadPrevious = Storage.exists(kSleepImagePath);
  if (hadPrevious && !Storage.rename(kSleepImagePath, kSleepImageBackupPath)) {
    Storage.remove(kSleepImagePartPath);
    return false;
  }
  if (!Storage.rename(kSleepImagePartPath, kSleepImagePath)) {
    if (hadPrevious) Storage.rename(kSleepImageBackupPath, kSleepImagePath);
    return false;
  }

  const uint8_t previousMode = SETTINGS.sleepScreen;
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  if (!SETTINGS.saveToFile()) {
    SETTINGS.sleepScreen = previousMode;
    (void)SETTINGS.saveToFile();
    Storage.remove(kSleepImagePath);
    if (hadPrevious) Storage.rename(kSleepImageBackupPath, kSleepImagePath);
    return false;
  }

  Storage.remove(kSleepImageBackupPath);
  return true;
}

bool AirPageWallpaper::installOverlay(const char* path, const char* destination) {
  if (!path || !destination || (strcmp(destination, kOverlayBmp) != 0 && strcmp(destination, kOverlayPng) != 0))
    return false;
  recoverInterruptedTransaction();
  const bool png = strcmp(destination, kOverlayPng) == 0;
  if (strcmp(path, destination) == 0) return validOverlay(destination, png);
  const std::string part = std::string(destination) + ".part";
  const std::string backup = std::string(destination) + ".bak";
  const char* alternate = png ? kOverlayBmp : kOverlayPng;
  const std::string alternateBackup = std::string(alternate) + ".bak";
  if (!copyFile(path, part.c_str()) || !validOverlay(part.c_str(), png)) {
    Storage.remove(part.c_str());
    return false;
  }
  const bool hadPrevious = Storage.exists(destination);
  const bool hadAlternate = Storage.exists(alternate);
  if (hadPrevious && !Storage.rename(destination, backup.c_str())) {
    Storage.remove(part.c_str());
    return false;
  }
  if (hadAlternate && !Storage.rename(alternate, alternateBackup.c_str())) {
    if (hadPrevious) Storage.rename(backup.c_str(), destination);
    Storage.remove(part.c_str());
    return false;
  }
  if (!Storage.rename(part.c_str(), destination) || !SETTINGS.saveToFile()) {
    Storage.remove(destination);
    if (hadPrevious) Storage.rename(backup.c_str(), destination);
    if (hadAlternate) Storage.rename(alternateBackup.c_str(), alternate);
    Storage.remove(part.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  Storage.remove(alternateBackup.c_str());
  return true;
}

void AirPageWallpaper::recoverInterruptedTransaction() {
  // A complete validated overlay can be used after interruption; otherwise
  // recover the prior images. No settings or source image are deleted.
  const bool hasOverlay = validOverlay(kOverlayPng, true) || validOverlay(kOverlayBmp, false);
  for (const char* overlay : {kOverlayBmp, kOverlayPng}) {
    const std::string part = std::string(overlay) + ".part";
    const std::string backup = std::string(overlay) + ".bak";
    Storage.remove(part.c_str());
    if (!Storage.exists(backup.c_str())) continue;
    if (hasOverlay)
      Storage.remove(backup.c_str());
    else {
      Storage.remove(overlay);
      if (!Storage.rename(backup.c_str(), overlay)) LOG_ERR("AIRP", "Could not recover overlay %s", overlay);
    }
  }
  Storage.remove(kSleepImagePartPath);
  if (!Storage.exists(kSleepImageBackupPath)) return;

  ImageInfo current;
  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM &&
      AirPageImageStore::inspectImage(kSleepImagePath, current) && current.format == ImageFormat::Bmp) {
    Storage.remove(kSleepImageBackupPath);
    return;
  }

  Storage.remove(kSleepImagePath);
  if (!Storage.rename(kSleepImageBackupPath, kSleepImagePath)) {
    LOG_ERR("AIRP", "Could not recover previous sleep screen");
  }
}

}  // namespace airpage
