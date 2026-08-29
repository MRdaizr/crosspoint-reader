#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
// The TOC is backed by an SD-card LUT, so getTocItem() returns a temporary
// value. Keep one bounded scratch string in the getter context instead of
// concatenating the entire look-ahead window into a second large allocation.
// FontCacheManager consumes each returned string synchronously.
struct ChapterPrewarmContext {
  const Epub* epub;
  int startIndex;
  int itemCount;
  std::string scratch;
};

const char* chapterPrewarmGetter(const void* context, uint32_t index) {
  auto* ctx = static_cast<ChapterPrewarmContext*>(const_cast<void*>(context));
  if (ctx == nullptr || ctx->epub == nullptr || index >= static_cast<uint32_t>(ctx->itemCount)) {
    return nullptr;
  }

  const auto item = ctx->epub->getTocItem(ctx->startIndex + static_cast<int>(index));
  // Indentation is made of ASCII spaces and is already covered by the UI
  // font. Prewarm the actual title, which is where CJK fallback glyphs occur.
  ctx->scratch = item.title;
  return ctx->scratch.c_str();
}
}  // namespace

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  sdFontSystem.ensureLoaded(renderer);

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto tocItem = epub->getTocItem(selectorIndex);
    if (tocItem.spineIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  const int pageItems = std::max(1, UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false));
  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  // Prewarm a bounded look-ahead window rather than only the visible page.
  // Fast chapter-list paging then reuses the same CJK fallback glyphs instead
  // of synchronously rereading the SD font for every newly exposed row.
  const int prewarmStartIndex = std::max(0, pageStartIndex - pageItems);
  const int prewarmEndIndex = std::min(totalItems, pageStartIndex + pageItems * 2);

  // Resolve the SD fallback from the first CJK title in the window. The
  // previous implementation built one concatenated string for this pass;
  // that allocation grew with the whole TOC window and was the main source of
  // heap pressure on long CJK books.
  int chapterTitleFontId = 0;
  for (int i = prewarmStartIndex; i < prewarmEndIndex; i++) {
    const auto item = epub->getTocItem(i);
    const int candidate = DynamicFont::fontForCjkText(renderer, item.title.c_str(), 0);
    if (renderer.isSdCardFont(candidate)) {
      chapterTitleFontId = candidate;
      break;
    }
  }

  const int prewarmCount = std::max(0, prewarmEndIndex - prewarmStartIndex);
  if (chapterTitleFontId != 0 && prewarmCount > 0) {
    ChapterPrewarmContext prewarmContext{epub.get(), prewarmStartIndex, prewarmCount, {}};
    DynamicFont::prewarmIfSdFont(renderer, chapterTitleFontId, chapterPrewarmGetter, &prewarmContext,
                                 static_cast<uint32_t>(prewarmCount));
    // GUI.drawList() truncates long labels with an ellipsis.
    DynamicFont::prewarmIfSdFont(renderer, chapterTitleFontId, "\xe2\x80\xa6");
  }

  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = epub->getTocItem(index);
                 std::string indent((item.level - 1) * 2, ' ');
                 return indent + item.title;
               },
               nullptr, nullptr, nullptr, false, nullptr, chapterTitleFontId);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
