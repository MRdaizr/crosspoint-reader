#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

int HomeActivity::getMenuItemCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Lyra/RoundedRaff can expose the first recent book as a single
  // "Continue Reading" row.  It must not reserve one hidden row per recent
  // book, otherwise button wrapping and FUI hit values drift from the drawn
  // menu.
  int count = (metrics.homeContinueReadingInMenu && !recentBooks.empty()) ? 1 : static_cast<int>(recentBooks.size());
  count += 5;  // File Browser, Recents, File transfer, Extensions, Settings
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

int HomeActivity::getMenuListCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int count = 5 + (hasOpdsServers ? 1 : 0);
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) ++count;
  return count;
}

int HomeActivity::menuSelectionIndex() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) return selectorIndex;
  return selectorIndex - static_cast<int>(recentBooks.size());
}

int HomeActivity::selectionIndexForMenuRow(const int row) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) return row;
  return static_cast<int>(recentBooks.size()) + row;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  resetUi();
  app.on(ACTION_FUI_MENU, &HomeActivity::onFuiMenu, this);
  app.setScreen(&HomeActivity::fuiScreen, this);
  fuiNav.reset();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const int base = (metrics.homeContinueReadingInMenu && !recentBooks.empty()) ? 0 : static_cast<int>(recentBooks.size());
  int menuIndex = menuItemToIndex(initialMenuItem, hasOpdsServers);
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty() && initialMenuItem != HomeMenuItem::NONE) ++menuIndex;
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuIndex;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  closeRouting();
  fuiMenuLabels.clear();
  fuiMenuItems.clear();
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

void HomeActivity::fuiScreen(UiScreen& screen, void* user) {
  static_cast<HomeActivity*>(user)->buildFuiScreen(screen);
}

void HomeActivity::onFuiMenu(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<HomeActivity*>(user);
  const int row = event.value;
  if (row < 0 || row >= self->getMenuListCount()) return;
  self->selectorIndex = self->selectionIndexForMenuRow(row);
  self->fuiNav.selected = row;
  self->app.clearTapFlash();
  self->activateSelected();
}

void HomeActivity::buildFuiScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  const int menuCount = getMenuListCount();
  fuiMenuLabels.clear();
  fuiMenuLabels.reserve(static_cast<size_t>(menuCount));
  std::vector<UIIcon> menuIcons;
  menuIcons.reserve(static_cast<size_t>(menuCount));
  std::vector<FuiMenuIconSlot> menuIconSlots;
  menuIconSlots.reserve(static_cast<size_t>(menuCount));
  fuiMenuItems.clear();
  fuiMenuItems.reserve(static_cast<size_t>(menuCount));

  // The row order mirrors the legacy theme menu.  Recent covers remain a
  // renderer-owned surface above; only the actual menu rows are published to
  // FUI so taps cannot select a cover through a stale list hit rectangle.
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    fuiMenuLabels.emplace_back(tr(STR_CONTINUE_READING));
    menuIcons.push_back(Book);
    menuIconSlots.push_back(FuiMenuIconSlot::HomeContinueReading);
  }
  fuiMenuLabels.emplace_back(tr(STR_BROWSE_FILES));
  menuIcons.push_back(Folder);
  menuIconSlots.push_back(FuiMenuIconSlot::HomeBrowseFiles);
  fuiMenuLabels.emplace_back(tr(STR_MENU_RECENT_BOOKS));
  menuIcons.push_back(Recent);
  menuIconSlots.push_back(FuiMenuIconSlot::HomeRecents);
  if (hasOpdsServers) {
    fuiMenuLabels.emplace_back(tr(STR_OPDS_BROWSER));
    menuIcons.push_back(Library);
    menuIconSlots.push_back(FuiMenuIconSlot::HomeOpds);
  }
  fuiMenuLabels.emplace_back(tr(STR_FILE_TRANSFER));
  menuIcons.push_back(Transfer);
  menuIconSlots.push_back(FuiMenuIconSlot::HomeFileTransfer);
  fuiMenuLabels.emplace_back(tr(STR_EXTENSIONS));
  menuIcons.push_back(Library);
  menuIconSlots.push_back(FuiMenuIconSlot::HomeExtensions);
  fuiMenuLabels.emplace_back(tr(STR_SETTINGS_TITLE));
  menuIcons.push_back(Settings);
  menuIconSlots.push_back(FuiMenuIconSlot::HomeSettings);

  for (size_t i = 0; i < fuiMenuLabels.size(); ++i) {
    fui::ListItem item;
    item.label = fuiMenuLabels[i].c_str();
    item.icon = GUI.showsFuiMenuIcon(menuIconSlots[i]) ? listIconFor(menuIcons[i], 24) : fui::BitmapRef{};
    item.actionValue = static_cast<int16_t>(i);
    fuiMenuItems.push_back(item);
  }

  const int selected = std::clamp(menuSelectionIndex(), 0, std::max(0, menuCount - 1));
  fuiNav.selected = selected;
  fuiMenuProps = {};
  fuiMenuProps.items = fuiMenuItems.data();
  fuiMenuProps.count = static_cast<uint16_t>(fuiMenuItems.size());
  fuiMenuProps.action = ACTION_FUI_MENU;
  fuiMenuProps.inputMask = fui::InputTouch;
  fuiMenuProps.labelText = screen.theme().bodyText;
  fuiMenuProps.valueInset = 8;
  fuiMenuProps.selectedIndex = static_cast<int16_t>(selected);
  fuiMenuProps.rowHeight = static_cast<int16_t>(mappedInput.hasTouch()
                                                    ? screen.theme().rowHeight
                                                    : metrics.listRowHeight);
  fuiNav.syncToProps(screen.body(), fuiMenuProps.rowHeight, screen.theme().listRowGap, menuCount, fuiMenuProps);
  screen.list(fuiMenuProps);
}

bool HomeActivity::routeFuiTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed) {
    if (app.invalidated()) requestUpdate();
    return static_cast<bool>(route.event);
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::Up && swipe != MappedInputManager::SwipeDir::Down) return false;
  const int count = getMenuListCount();
  const int delta = swipe == MappedInputManager::SwipeDir::Up ? fuiNav.pageRows() : -fuiNav.pageRows();
  if (fuiNav.scrollBy(delta, count)) requestUpdate();
  return true;
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (routeFuiTouch()) return;

  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
  }
}

void HomeActivity::activateSelected() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool continueRow = metrics.homeContinueReadingInMenu && !recentBooks.empty();
  if ((!continueRow && selectorIndex < static_cast<int>(recentBooks.size())) || (continueRow && selectorIndex == 0)) {
    if (!recentBooks.empty()) onSelectBook(recentBooks[0].path);
    return;
  }

  const int menuIndex = continueRow ? selectorIndex - 1 : selectorIndex - static_cast<int>(recentBooks.size());
  switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
    case HomeMenuItem::FILE_BROWSER:
      onFileBrowserOpen();
      break;
    case HomeMenuItem::RECENTS:
      onRecentsOpen();
      break;
    case HomeMenuItem::OPDS_BROWSER:
      onOpdsBrowserOpen();
      break;
    case HomeMenuItem::FILE_TRANSFER:
      onFileTransferOpen();
      break;
    case HomeMenuItem::EXTENSIONS:
      onExtensionsOpen();
      break;
    case HomeMenuItem::SETTINGS_MENU:
      onSettingsOpen();
      break;
    default:
      break;
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // The cover/tile area remains renderer-owned, while the static menu below
  // it is a regular FUI list.  This keeps the existing theme-specific cover
  // artwork and buffer reuse without retaining the legacy menu hit testing.
  renderUi();

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onExtensionsOpen() { activityManager.goToExtensions(); }
