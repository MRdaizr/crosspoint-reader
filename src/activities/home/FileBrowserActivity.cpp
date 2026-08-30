#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

// Row caches are built before the display helpers' definitions below, so keep
// their declarations visible to rebuildRowItems().
std::string getFileName(std::string filename);
std::string getFileExtension(std::string filename);

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    rebuildRowItems();
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    rebuildRowItems();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
  rebuildRowItems();
}

void FileBrowserActivity::rebuildRowItems() {
  rowsShowFileIcons = UITheme::getInstance().getTheme().showsFileIcons();
  rowLabels.resize(files.size());
  rowValues.resize(files.size());
  rowItems.clear();
  rowItems.reserve(files.size());

  for (size_t i = 0; i < files.size(); ++i) {
    rowLabels[i] = getFileName(files[i]);
    rowValues[i] = getFileExtension(files[i]);

    fui::ListItem item;
    item.label = rowLabels[i].c_str();
    item.value = rowValues[i].empty() ? nullptr : rowValues[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    // RoundedRaffExt and the current text-first menu intentionally suppress
    // all generic row icons.
    item.icon = {};
    rowItems.push_back(item);
  }
}

void FileBrowserActivity::onEnter() {
  UiListActivity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  const int listFontId = sdFontSystem.currentFontId();
  LOG_INF("FBR", "FileBrowser list font source=%s id=%d",
          renderer.isSdCardFont(listFontId) ? "SD" : "builtin", listFontId);

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  {
    RenderLock lock(*this);
    nav.selected = 0;

    auto root = Storage.open(basepath.c_str());
    if (!root) {
      basepath = "/";
      loadFiles();
    } else if (!root.isDirectory()) {
      lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

      const std::string oldPath = basepath;
      basepath = FsHelpers::extractFolderPath(basepath);
      loadFiles();

      const auto pos = oldPath.find_last_of('/');
      const std::string fileName = oldPath.substr(pos + 1);
      nav.selected = static_cast<int>(findEntry(fileName));
    } else {
      loadFiles();
    }
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  UiListActivity::onExit();
  files.clear();
  rowLabels.clear();
  rowValues.clear();
  rowItems.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

bool FileBrowserActivity::handleCustomInput() {
  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  bool canGoHome = false;
  {
    RenderLock lock(*this);
    canGoHome = mode == Mode::Books && basepath != "/" && !lockLongPressBack;
  }
  if (canGoHome && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS) {
    {
      RenderLock lock(*this);
      closeRouting();
      basepath = "/";
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
      nav.follow(listCount());
    }
    requestUpdate();
    return true;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    lockLongPressBack = false;
    return true;
  }

  return false;
}

bool FileBrowserActivity::handleButtons() {
  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return true;
    }
    std::string entry;
    bool isDirectory = false;
    {
      RenderLock lock(*this);
      if (files.empty() || nav.selected >= files.size()) return true;
      entry = files[nav.selected];
      isDirectory = entry.back() == '/';
    }

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string selectedPath;
      {
        RenderLock lock(*this);
        selectedPath = basepath;
        if (selectedPath.back() != '/') selectedPath += "/";
      }
      ActivityResult res{FilePathResult{selectedPath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return true;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string fullPath;
      {
        RenderLock lock(*this);
        fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += entry;
      }

      auto handler = [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (removeDirFile(fullPath)) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            {
              RenderLock lock(*this);
              closeRouting();
              loadFiles();
              if (files.empty()) {
                nav.selected = 0;
              } else if (nav.selected >= files.size()) {
                // Move selection to the new "last" item
                nav.selected = static_cast<int>(files.size() - 1);
              }
              nav.follow(listCount());
            }

            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return true;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (isDirectory) {
        {
          RenderLock lock(*this);
          closeRouting();
          if (basepath.back() != '/') basepath += "/";
          basepath += entry.substr(0, entry.length() - 1);
          loadFiles();
          nav.selected = 0;
          nav.top = 0;
          nav.follow(listCount());
        }
        requestUpdate();
      } else {
        std::string bookPath;
        {
          RenderLock lock(*this);
          if (basepath.back() != '/') basepath += "/";
          bookPath = basepath + entry;
        }
        onSelectBook(bookPath);
      }
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      bool atRoot = false;
      {
        RenderLock lock(*this);
        atRoot = basepath == "/";
      }
      if (!atRoot) {
        {
          RenderLock lock(*this);
          closeRouting();
          const std::string oldPath = basepath;

          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          nav.selected = static_cast<int>(findEntry(dirName));
          nav.follow(listCount());
        }

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
    return true;
  }
  return false;
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, folderName.c_str());
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // FUI's font fields are slots, while DynamicFont returns the concrete
  // GfxRenderer/SD-card font ID.  Start each build from the bundled UI faces;
  // the complete file list below will rebind the slots to the selected SD
  // font when one is available, including Latin-only names.
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, UI_10_FONT_ID);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  const int pathFontId = DynamicFont::fontForCjkText(renderer, basepath.c_str(), SMALL_FONT_ID);
  const int pathReserved = renderer.getLineHeight(pathFontId) + metrics.verticalSpacing;
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight + pathReserved), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (files.empty()) {
    const char* emptyMessage = mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    const int emptyFontId = DynamicFont::fontForSdCardText(renderer, UI_12_FONT_ID);
    uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, emptyFontId);
    fui::TextStyle emptyStyle = screen.theme().bodyText;
    emptyStyle.bold = !renderer.isSdCardFont(emptyFontId);
    screen.centeredText(emptyMessage, emptyStyle);
    return;
  }
  // Folder labels depend on the active theme (RoundedRaffExt uses the
  // bracketed text form when icons are disabled).  Rebuild only for a theme
  // change or if a defensive size check detects stale data; normal repaints
  // reuse stable pointers into rowLabels/rowValues.
  if (rowItems.size() != files.size() || rowsShowFileIcons != UITheme::getInstance().getTheme().showsFileIcons()) {
    rebuildRowItems();
  }

  // A FUI list has one shared label slot.  Bind it to the selected SD face
  // for every row, not only when a filename contains CJK.  This keeps Latin,
  // Chinese and mixed directories on the same dynamic font path.
  const int listFontId = DynamicFont::fontForSdCardText(renderer, UI_10_FONT_ID);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, listFontId);

  fui::ListProps props;
  props.items = rowItems.data(); props.count = static_cast<uint16_t>(rowItems.size()); props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch; props.valueInset = 8; props.labelText = screen.theme().smallText; props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  if (renderer.isSdCardFont(listFontId)) {
    const int first = std::clamp(nav.top, 0, static_cast<int>(rowLabels.size()));
    const int count = std::min(static_cast<int>(rowLabels.size()) - first, std::max(1, nav.visibleRows));
    for (int i = 0; i < count; ++i) {
      const size_t row = static_cast<size_t>(first + i);
      const int missed = DynamicFont::prewarmIfSdFont(renderer, listFontId, rowLabels[row]);
      if (missed > 0) {
        LOG_INF("FBR", "row=%d SD glyph miss: nameBytes=%u missed=%d name='%s'",
                first + i, static_cast<unsigned>(rowLabels[row].size()), missed, rowLabels[row].c_str());
      }
    }
    DynamicFont::prewarmIfSdFont(renderer, listFontId, "\xe2\x80\xa6");
  }
  screen.list(props);
}

void FileBrowserActivity::activateIndex(const int index) {
  std::string entry;
  std::string currentPath;
  bool isDirectory = false;
  {
    RenderLock lock(*this);
    if (index < 0 || index >= static_cast<int>(files.size())) return;
    nav.selected = index;
    entry = files[static_cast<size_t>(index)];
    currentPath = basepath;
    isDirectory = !entry.empty() && entry.back() == '/';
  }

  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string selectedPath = currentPath;
    if (selectedPath.back() != '/') selectedPath += "/";
    setResult(FilePathResult{selectedPath + entry});
    finish();
    return;
  }
  if (isDirectory) {
    {
      RenderLock lock(*this);
      closeRouting();
      if (basepath.back() != '/') basepath += "/";
      basepath += entry.substr(0, entry.length() - 1);
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
      nav.follow(listCount());
    }
    requestUpdate();
  } else {
    std::string bookPath = currentPath;
    if (bookPath.back() != '/') bookPath += "/";
    onSelectBook(bookPath + entry);
  }
}

void FileBrowserActivity::drawFooter() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pathFontId = DynamicFont::fontForCjkText(renderer, basepath.c_str(), SMALL_FONT_ID);
  const int pathLineHeight = renderer.getLineHeight(pathFontId);
  const int pathY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
  const int separatorY = pathY - metrics.verticalSpacing / 2;
  renderer.drawLine(0, separatorY, renderer.getScreenWidth() - 1, separatorY, 3, true);
  const int pathMaxWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const char* pathDisplay = basepath.c_str();
  char leftTruncBuf[256];
  if (renderer.getTextWidth(pathFontId, basepath.c_str()) > pathMaxWidth) {
    const char ellipsis[] = "\xe2\x80\xa6";
    const int available = pathMaxWidth - renderer.getTextWidth(pathFontId, ellipsis);
    const char* p = basepath.c_str();
    while (*p && renderer.getTextWidth(pathFontId, p) > available) {
      ++p;
      while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
    }
    snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
    pathDisplay = leftTruncBuf;
  }
  renderer.drawText(pathFontId, metrics.contentSidePadding, pathY, pathDisplay);
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  const bool hasSelectedFile = nav.selected >= 0 && nav.selected < static_cast<int>(files.size());
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && hasSelectedFile && !files[nav.selected].empty() &&
                                     files[nav.selected].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP), files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
