#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFont.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_MANIFEST_BYTES = 64 * 1024;
constexpr size_t MAX_MANIFEST_FAMILIES = 64;
constexpr size_t MAX_MANIFEST_FILES_PER_FAMILY = 16;
constexpr char MANIFEST_TMP[] = "/.crosspoint/fonts_manifest.tmp";

#define FONT_MANIFEST_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_STRINGIFY(x) FONT_MANIFEST_STRINGIFY_INNER(x)
#ifndef FONTS_MANIFEST_VERSION
#define FONTS_MANIFEST_VERSION 1
#endif
#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

bool readManifestString(JsonVariantConst value, std::string& out, const size_t maxLength = 96) {
  const char* text = value | "";
  if (!text || text[0] == '\0' || std::strlen(text) > maxLength) return false;
  out = text;
  return true;
}
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), installer_(sdFontSystem.registry()) {}

bool FontDownloadActivity::preventAutoSleep() {
  return state_ == State::Loading || state_ == State::Downloading || state_ == State::Complete ||
         state_ == State::Error;
}

int FontDownloadActivity::listCount() const {
  if (state_ == State::Installed) return static_cast<int>(families_.size()) + 1;
  if (state_ == State::Remote) return static_cast<int>(remoteFamilies_.size());
  return 0;
}

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  state_ = State::Installed;
  errorMessage_.clear();
  refreshFamilies();
}

void FontDownloadActivity::refreshFamilies() {
  installer_.refreshRegistry();
  families_.clear();
  families_.reserve(sdFontSystem.registry().getFamilies().size());
  for (const auto& family : sdFontSystem.registry().getFamilies()) families_.push_back(family.name);
  rowValues_.assign(static_cast<size_t>(listCount()), std::string());
  rowItems_.clear();
  rowItems_.reserve(rowValues_.size());

  fui::ListItem downloadItem;
  downloadItem.label = tr(STR_DOWNLOAD_FONTS);
  downloadItem.actionValue = 0;
  downloadItem.icon = {};
  rowItems_.push_back(downloadItem);
  for (size_t i = 0; i < families_.size(); ++i) {
    fui::ListItem item;
    item.label = families_[i].c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    item.icon = {};
    rowItems_.push_back(item);
    rowValues_[i + 1] = families_[i] == SETTINGS.sdFontFamilyName ? tr(STR_SELECTED) : "";
  }
  nav.selected = std::min(nav.selected, std::max(0, listCount() - 1));
  requestUpdate();
}

void FontDownloadActivity::rebuildRemoteRows() {
  rowValues_.assign(remoteFamilies_.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(remoteFamilies_.size());
  for (size_t i = 0; i < remoteFamilies_.size(); ++i) {
    const auto& family = remoteFamilies_[i];
    fui::ListItem item;
    item.label = family.name.c_str();
    item.subtitle = family.description.empty() ? nullptr : family.description.c_str();
    if (family.hasUpdate) {
      item.value = tr(STR_UPDATE_AVAILABLE);
    } else if (family.installed) {
      item.value = tr(STR_INSTALLED);
      item.state = fui::StateDisabled;
    } else {
      rowValues_[i] = formatSize(family.totalSize);
      item.value = rowValues_[i].c_str();
    }
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems_.push_back(item);
  }
  nav.selected = std::min(nav.selected, std::max(0, listCount() - 1));
}

const char* FontDownloadActivity::headerTitle() const {
  return state_ == State::Installed ? tr(STR_FONT_DOWNLOAD) : tr(STR_FONT_BROWSER);
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  if (state_ == State::Loading) {
    screen.centeredText(tr(STR_LOADING_FONT_LIST), screen.theme().bodyText);
    return;
  }
  if (state_ == State::Downloading) {
    const std::string label = std::string(tr(STR_DOWNLOADING)) + " " +
                              (downloadingFamilyIndex_ >= 0 &&
                                       downloadingFamilyIndex_ < static_cast<int>(remoteFamilies_.size())
                                   ? remoteFamilies_[downloadingFamilyIndex_].name
                                   : std::string());
    screen.centeredText(label.c_str(), screen.theme().bodyText);
    return;
  }
  if (state_ == State::Complete) {
    screen.centeredText(tr(STR_FONT_INSTALLED), screen.theme().bodyText);
    return;
  }
  if (state_ == State::Error) {
    screen.centeredText(errorMessage_.empty() ? tr(STR_FONT_INSTALL_FAILED) : errorMessage_.c_str(),
                        screen.theme().bodyText);
    return;
  }
  if (rowItems_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }
  if (state_ == State::Installed) {
    for (size_t i = 1; i < rowItems_.size() && i < rowValues_.size(); ++i) {
      rowValues_[i] = families_[i - 1] == SETTINGS.sdFontFamilyName ? tr(STR_SELECTED) : "";
      rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    }
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.subtitleText = screen.theme().smallText;
  syncListViewport(screen, props, state_ == State::Remote);
  screen.list(props);
}

void FontDownloadActivity::activateIndex(const int index) {
  if (state_ == State::Installed) {
    if (index == 0) {
      app.clearTapFlash();
      openRemoteBrowser();
      return;
    }
    const int familyIndex = index - 1;
    if (familyIndex >= 0 && familyIndex < static_cast<int>(families_.size())) confirmDelete(familyIndex);
    return;
  }
  if (state_ == State::Remote && index >= 0 && index < static_cast<int>(remoteFamilies_.size())) {
    app.clearTapFlash();
    downloadFamily(index);
  }
}

void FontDownloadActivity::onBackButton() {
  if (state_ == State::Remote || state_ == State::Error || state_ == State::Complete) {
    state_ = State::Installed;
    refreshFamilies();
    return;
  }
  finish();
}

bool FontDownloadActivity::handleCustomInput() {
  if (state_ == State::Complete || state_ == State::Error) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      state_ = State::Installed;
      refreshFamilies();
    }
    return true;
  }
  return state_ == State::Loading || state_ == State::Downloading;
}

void FontDownloadActivity::openRemoteBrowser() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    requestUpdate();
    return;
  }
  state_ = State::Loading;
  requestUpdateAndWait();
  if (!fetchManifest()) {
    state_ = State::Error;
    requestUpdate();
    return;
  }
  state_ = State::Remote;
  nav.reset();
  rebuildRemoteRows();
  requestUpdate();
}

bool FontDownloadActivity::fetchManifest() {
  remoteFamilies_.clear();
  remoteBaseUrl_.clear();
  errorMessage_.clear();
  Storage.remove(MANIFEST_TMP);
  const auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP);
  if (result != HttpDownloader::OK) {
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    LOG_ERR("FONT", "Failed to fetch manifest");
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, file)) {
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  const size_t manifestSize = file.fileSize();
  if (manifestSize > MAX_MANIFEST_BYTES) {
    LOG_ERR("FONT", "Manifest too large: %zu bytes", manifestSize);
    file.close();
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    return false;
  }
  JsonDocument doc;
  const auto parseError = deserializeJson(doc, file);
  file.close();
  Storage.remove(MANIFEST_TMP);
  if (parseError) {
    LOG_ERR("FONT", "Manifest parse error: %s", parseError.c_str());
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    return false;
  }
  if ((doc["version"] | 0) != FONTS_MANIFEST_VERSION || !readManifestString(doc["baseUrl"], remoteBaseUrl_, 192)) {
    LOG_ERR("FONT", "Unsupported or malformed font manifest");
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    return false;
  }
  if (remoteBaseUrl_.back() != '/') remoteBaseUrl_.push_back('/');
  JsonArray families = doc["families"].as<JsonArray>();
  if (families.isNull()) {
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    return false;
  }
  installer_.refreshRegistry();
  const size_t count = std::min(families.size(), MAX_MANIFEST_FAMILIES);
  remoteFamilies_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    JsonObject obj = families[i].as<JsonObject>();
    RemoteFamily family;
    if (!readManifestString(obj["name"], family.name, 31)) continue;
    readManifestString(obj["description"], family.description, 128);
    JsonArray files = obj["files"].as<JsonArray>();
    if (files.isNull() || files.size() == 0 || files.size() > MAX_MANIFEST_FILES_PER_FAMILY) continue;
    for (JsonVariant value : files) {
      JsonObject fileObj = value.as<JsonObject>();
      RemoteFile remoteFile;
      if (!readManifestString(fileObj["name"], remoteFile.name, 63) ||
          !FontInstaller::isValidCpfontFilename(remoteFile.name.c_str())) {
        family.files.clear();
        break;
      }
      remoteFile.size = fileObj["size"] | static_cast<size_t>(0);
      remoteFile.crc32 = fileObj["crc32"] | static_cast<uint32_t>(0);
      if (remoteFile.size == 0 || remoteFile.size > 4 * 1024 * 1024) {
        family.files.clear();
        break;
      }
      family.totalSize += remoteFile.size;
      family.files.push_back(std::move(remoteFile));
    }
    if (family.files.empty()) continue;
    family.installed = installer_.isFamilyInstalled(family.name.c_str());
    if (family.installed) {
      for (const auto& remoteFile : family.files) {
        char path[160] = {};
        FontInstaller::buildFontPath(family.name.c_str(), remoteFile.name.c_str(), path, sizeof(path));
        HalFile installed;
        if (!Storage.openFileForRead("FONT", path, installed) || installed.fileSize() != remoteFile.size) {
          family.hasUpdate = true;
          installed.close();
          break;
        }
        installed.close();
      }
    }
    remoteFamilies_.push_back(std::move(family));
  }
  return !remoteFamilies_.empty();
}

void FontDownloadActivity::downloadFamily(const int index) {
  if (index < 0 || index >= static_cast<int>(remoteFamilies_.size())) return;
  auto& family = remoteFamilies_[index];
  downloadingFamilyIndex_ = index;
  downloadingFileCount_ = family.files.size();
  downloadingFileIndex_ = 0;
  cancelRequested_ = false;
  state_ = State::Downloading;
  requestUpdateAndWait();
  if (!installer_.ensureFamilyDir(family.name.c_str())) {
    state_ = State::Error;
    errorMessage_ = tr(STR_FONT_INSTALL_FAILED);
    requestUpdate();
    return;
  }
  for (const auto& remoteFile : family.files) {
    char path[160] = {};
    FontInstaller::buildFontPath(family.name.c_str(), remoteFile.name.c_str(), path, sizeof(path));
    const std::string url = remoteBaseUrl_ + remoteFile.name;
    fileProgress_ = 0;
    fileTotal_ = remoteFile.size;
    const auto result = HttpDownloader::downloadToFile(
        url, path,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
              mappedInput.isPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          requestUpdate(true);
        },
        &cancelRequested_);
    if (result != HttpDownloader::OK || !installer_.validateCpfontFile(path)) {
      installer_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      state_ = result == HttpDownloader::ABORTED ? State::Remote : State::Error;
      errorMessage_ = tr(STR_FONT_INSTALL_FAILED);
      requestUpdate();
      return;
    }
    uint32_t crc = 0;
    if (!computeFileCrc32(path, crc) || crc != remoteFile.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s", remoteFile.name.c_str());
      installer_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      state_ = State::Error;
      errorMessage_ = tr(STR_FONT_INSTALL_FAILED);
      requestUpdate();
      return;
    }
    downloadingFileIndex_++;
  }
  installer_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;
  sdFontSystem.markRegistryDirty();
  sdFontSystem.ensureLoaded(renderer);
  state_ = State::Complete;
  requestUpdate();
}

bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) const {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) return false;
  uint8_t buffer[256] = {};
  uint32_t crc = 0;
  while (file.available()) {
    const int bytes = file.read(buffer, sizeof(buffer));
    if (bytes <= 0) break;
    crc = esp_rom_crc32_le(crc, buffer, static_cast<uint32_t>(bytes));
  }
  file.close();
  outCrc = crc;
  return true;
}

void FontDownloadActivity::confirmDelete(const int index) {
  if (index < 0 || index >= static_cast<int>(families_.size())) return;
  const std::string family = families_[index];
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), family),
      [this, index](const ActivityResult& result) { handleDeleteResult(index, result); });
}

void FontDownloadActivity::handleDeleteResult(const int index, const ActivityResult& result) {
  if (result.isCancelled || index < 0 || index >= static_cast<int>(families_.size())) return;
  if (installer_.deleteFamily(families_[index].c_str()) != FontInstaller::Error::OK) {
    LOG_ERR("FONT", "Failed to delete SD font family %s", families_[index].c_str());
    return;
  }
  sdFontSystem.markRegistryDirty();
  sdFontSystem.ensureLoaded(renderer);
  refreshFamilies();
}

std::string FontDownloadActivity::formatSize(const size_t bytes) {
  char buffer[32] = {};
  if (bytes >= 1024 * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  }
  return buffer;
}
