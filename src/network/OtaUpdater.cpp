#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareFlasher.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser. Buffering the whole
  // body would add a growing allocation on top of the TLS session's heap.
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  if (latestMajor != currentMajor) return latestMajor > currentMajor;
  if (latestMinor != currentMinor) return latestMinor > currentMinor;
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // Treat an equal-segment release as newer when running an RC build.
  if (strstr(currentVersion, "-rc") != nullptr) return true;
  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  const esp_partition_t* otaPartition = esp_ota_get_next_update_partition(nullptr);
  if (!otaPartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  const size_t imageSize = otaSize > 0 ? otaSize : OTA_SIZE_UNKNOWN;
  esp_err_t espErr = esp_ota_begin(otaPartition, imageSize, &otaHandle);
  if (espErr != ESP_OK) {
    LOG_DBG("OTA", "OTA begin failed: %s", esp_err_to_name(espErr));
    return INTERNAL_UPDATE_ERROR;
  }

  // Disable Wi-Fi power saving while the OTA stream is active.
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  bool wrongChip = false;

  // Buffer the first 14 bytes so chip_id (esp_image_header_t offset 12) is
  // checked before any byte is written to the OTA partition.
  uint8_t header[14] = {};
  size_t headerLength = 0;
  bool headerValidated = false;

  auto writeChunk = [&](const uint8_t* data, size_t length) -> bool {
    if (length == 0) return true;
    if (esp_ota_write(otaHandle, data, length) != ESP_OK) {
      flashOk = false;
      return false;
    }
    processedSize += length;
    // Fire the callback only on whole-percent change. E-ink cannot repaint
    // faster than a percent tick and this avoids waking the render task for
    // every network chunk.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  };

  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t length) {
    if (!headerValidated) {
      const size_t take = std::min(length, sizeof(header) - headerLength);
      std::memcpy(header + headerLength, data, take);
      headerLength += take;
      if (headerLength < sizeof(header)) return true;

      uint16_t imageChip = 0;
      std::memcpy(&imageChip, header + 12, sizeof(imageChip));
      const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
      if (deviceChip != 0xFFFF && imageChip != deviceChip) {
        LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
        wrongChip = true;
        return false;
      }

      headerValidated = true;
      if (!writeChunk(header, sizeof(header))) return false;
      data += take;
      length -= take;
    }

    return writeChunk(data, length);
  });

  // Restore the default Wi-Fi power mode on every post-begin path.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongChip) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", fetchOk ? "flash write" : "download");
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }

  if (!headerValidated) {
    LOG_ERR("OTA", "Firmware download ended before image header was received");
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  // esp_ota_end performs the remaining ESP image integrity checks before the
  // boot partition is changed.
  espErr = esp_ota_end(otaHandle);
  if (espErr != ESP_OK) {
    LOG_ERR("OTA", "OTA end failed: %s", esp_err_to_name(espErr));
    return INTERNAL_UPDATE_ERROR;
  }

  espErr = esp_ota_set_boot_partition(otaPartition);
  if (espErr != ESP_OK) {
    LOG_ERR("OTA", "OTA boot partition switch failed: %s", esp_err_to_name(espErr));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
