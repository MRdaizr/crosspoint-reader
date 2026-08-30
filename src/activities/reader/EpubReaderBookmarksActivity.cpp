#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <util/BookmarkUtil.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/BookmarkFile.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  UiListActivity::onEnter();

  if (!epub) {
    return;
  }

  if (!BookmarkFile::load(epubPath, bookmarks)) {
    LOG_DBG("EPB", "No bookmark file found for %s, starting with empty bookmarks", epubPath.c_str());
    bookmarks.clear();
    bookmarks.shrink_to_fit();
  }
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());

}

bool EpubReaderBookmarksActivity::handleCustomInput() {
  if (confirmingDelete == 0 && !bookmarks.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    deleteIndex = nav.selected;
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
    return true;
  }
  return false;
}

void EpubReaderBookmarksActivity::onRowLongPress(const int index) {
  if (index >= 0 && index < static_cast<int>(bookmarks.size())) {
    deleteIndex = index;
    nav.selected = 0;
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
  }
}

bool EpubReaderBookmarksActivity::handleButtons() {
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      confirmingDelete = DELETE_MODE_OFF;
      requestUpdate();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;
        requestUpdate();
        return true;
      }
      if (deleteIndex >= 0 && deleteIndex < static_cast<int>(bookmarks.size())) {
        bookmarks.erase(bookmarks.begin() + deleteIndex);
        BookmarkFile::save(epubPath, bookmarks);
        if (bookmarks.empty()) {
          ActivityResult result;
          result.isCancelled = true;
          setResult(std::move(result));
          finish();
          return true;
        }
        nav.selected = std::min(deleteIndex, static_cast<int>(bookmarks.size()) - 1);
      }
      confirmingDelete = DELETE_MODE_OFF;
      requestUpdate();
      return true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return UiListActivity::handleButtons();
}

void EpubReaderBookmarksActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(bookmarks.size()) || !epub) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto& bookmark = bookmarks[static_cast<size_t>(index)];
  ProgressChangeResult result{};
  result.xpath = bookmark.xpath;
  result.percentage = bookmark.percentage;
  result.hasSavedProgress = true;
  result.hasVisibleTextOffset = bookmark.hasVisibleTextOffset;
  result.visibleTextOffset = bookmark.visibleTextOffset;
  result.spineIndex = bookmark.computedSpineIndex;
  if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
      bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
    result.page = bookmark.computedChapterProgress;
    result.totalPages = bookmark.computedChapterPageCount;
  } else {
    const CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage}, renderer);
    result.spineIndex = pos.spineIndex;
    result.page = pos.pageNumber;
  }
  setResult(std::move(result));
  finish();
}

void EpubReaderBookmarksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (bookmarks.empty()) {
    screen.centeredText(tr(STR_NO_BOOKMARKS), screen.theme().bodyText);
    return;
  }
  const int count = confirmingDelete ? 1 : static_cast<int>(bookmarks.size());
  rowLabels.clear(); rowSubtitles.clear(); rowItems.clear();
  rowLabels.reserve(static_cast<size_t>(count)); rowSubtitles.reserve(static_cast<size_t>(count)); rowItems.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const int source = confirmingDelete ? deleteIndex : i;
    const auto& bookmark = bookmarks[static_cast<size_t>(source)];
    rowLabels.push_back(bookmark.summary.empty() ? std::string(tr(STR_UNNAMED)) : bookmark.summary);
    const int tocIndex = epub ? epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex) : -1;
    const std::string tocTitle = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : tr(STR_UNNAMED);
    std::string subtitle = std::to_string(static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "% - ";
    if (bookmark.computedChapterPageCount > 0) subtitle += std::to_string(bookmark.computedChapterProgress + 1) + "/" + std::to_string(bookmark.computedChapterPageCount) + " - ";
    rowSubtitles.push_back(subtitle + tocTitle);
    fui::ListItem item;
    item.label = rowLabels.back().c_str(); item.subtitle = rowSubtitles.back().c_str();
    item.actionValue = static_cast<int16_t>(source); item.icon = {};
    rowItems.push_back(item);
  }
  fui::ListProps props;
  props.items = rowItems.data(); props.count = static_cast<uint16_t>(rowItems.size()); props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress; props.subtitleText = screen.theme().smallText; props.subtitleText.maxLines = 2;
  syncListViewport(screen, props, true); screen.list(props);
}

void EpubReaderBookmarksActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(confirmingDelete ? tr(STR_CANCEL) : tr(STR_BACK),
                                             confirmingDelete ? tr(STR_DELETE) : (bookmarks.empty() ? "" : tr(STR_SELECT)),
                                             tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
