#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

struct RecentBooksTextContext {
  const std::vector<RecentBook>* books;
  int startIndex;
};

const char* recentBookTitleGetter(const void* context, uint32_t index) {
  const auto* ctx = static_cast<const RecentBooksTextContext*>(context);
  const size_t itemIndex = static_cast<size_t>(ctx->startIndex) + index;
  return itemIndex < ctx->books->size() ? (*ctx->books)[itemIndex].title.c_str() : nullptr;
}

const char* recentBookAuthorGetter(const void* context, uint32_t index) {
  const auto* ctx = static_cast<const RecentBooksTextContext*>(context);
  const size_t itemIndex = static_cast<size_t>(ctx->startIndex) + index;
  return itemIndex < ctx->books->size() ? (*ctx->books)[itemIndex].author.c_str() : nullptr;
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  sdFontSystem.ensureLoaded(renderer);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    const int pageItems =
        std::max(1, UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true));
    const int pageStartIndex = static_cast<int>(selectorIndex) / pageItems * pageItems;
    std::string visibleText;
    for (int i = pageStartIndex; i < static_cast<int>(recentBooks.size()) && i < pageStartIndex + pageItems; i++) {
      visibleText += recentBooks[i].title;
      visibleText += '\n';
    }
    visibleText += "\xe2\x80\xa6";  // ellipsis used by truncatedText()
    const int recentTitleFontId = DynamicFont::fontForCjkText(renderer, visibleText.c_str(), 0);
    const int visibleCount = std::min(static_cast<int>(recentBooks.size()) - pageStartIndex, pageItems);
    const RecentBooksTextContext prewarmContext{&recentBooks, pageStartIndex};
    DynamicFont::prewarmIfSdFont(renderer, recentTitleFontId, recentBookTitleGetter, &prewarmContext,
                                 static_cast<uint32_t>(std::max(0, visibleCount)));
    // The renderer truncates titles with an ellipsis; keep that glyph resident
    // without rebuilding the title batch.
    DynamicFont::prewarmIfSdFont(renderer, recentTitleFontId, "\xe2\x80\xa6");

    // BaseTheme draws authors as per-row subtitles. Batch them first so those
    // row callbacks become resident-cache subset hits instead of repeatedly
    // reopening the .cpfont file.
    int recentAuthorFontId = 0;
    for (int i = pageStartIndex; i < pageStartIndex + visibleCount; i++) {
      const int candidate = DynamicFont::fontForCjkText(renderer, recentBooks[i].author.c_str(), SMALL_FONT_ID);
      if (renderer.isSdCardFont(candidate)) {
        recentAuthorFontId = candidate;
        break;
      }
    }
    if (recentAuthorFontId != 0) {
      DynamicFont::prewarmIfSdFont(renderer, recentAuthorFontId, recentBookAuthorGetter, &prewarmContext,
                                   static_cast<uint32_t>(visibleCount));
    }

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); }, nullptr, false, nullptr,
        recentTitleFontId);
  }

  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
