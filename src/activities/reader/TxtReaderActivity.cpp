#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "AchievementsStore.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "TxtReaderMenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/AchievementPopupUtils.h"

namespace {
// Most pages in a CJK TXT are below 1 KiB. Start there and only expand when a
// page really crosses the boundary; priming a fixed 4 KiB made every turn read
// metrics for hundreds of characters that would not appear on screen.
constexpr size_t INITIAL_LAYOUT_CHUNK_SIZE = 1024;
constexpr size_t LAYOUT_CHUNK_GROWTH = 1024;
constexpr size_t MAX_LAYOUT_CHUNK_SIZE = 4 * 1024;
constexpr size_t PAGE_LAYOUT_CACHE_SIZE = 3;
constexpr int APPROXIMATE_PAGE_KEY_BASE = -1000000;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 5;          // Increment when page offset calculation changes
constexpr uint32_t PARTIAL_CACHE_MAGIC = 0x54584249;  // "TXBI"
constexpr uint32_t PROGRESS_MAGIC = 0x54585052;       // "TXPR"
constexpr unsigned long INDEX_BUILD_TICK_MS = 80UL;
constexpr unsigned long INDEX_PROGRESS_REFRESH_MS = 2000UL;
constexpr uint8_t INDEX_PROGRESS_STEP = 5;
constexpr size_t INDEX_CHECKPOINT_PAGE_STEP = 8;

size_t fittingPrefix(const GfxRenderer& renderer, const int fontId, const std::string& text, const int maxWidth) {
  size_t position = 0;
  size_t lastSpace = 0;

  const auto nextCodepoint = [&text](const size_t begin) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(text.data() + begin);
    utf8NextCodepoint(&cursor);
    const size_t end = static_cast<size_t>(cursor - reinterpret_cast<const unsigned char*>(text.data()));
    return end > begin && end <= text.size() ? end : begin + 1;
  };
  const auto rememberSpaces = [&text, &lastSpace](const size_t begin, const size_t end) {
    const size_t space = text.rfind(' ', end - 1);
    if (space != std::string::npos && space >= begin) lastSpace = space;
  };

  while (position < text.size()) {
    size_t probeEnd = position;
    for (int count = 0; count < 16 && probeEnd < text.size(); ++count) {
      probeEnd = nextCodepoint(probeEnd);
    }
    if (renderer.getTextAdvanceX(fontId, text.substr(0, probeEnd).c_str(), EpdFontFamily::REGULAR) <= maxWidth) {
      rememberSpaces(position, probeEnd);
      position = probeEnd;
      continue;
    }

    // The 16-character probe crossed the boundary. Refine only this small
    // range, retaining exact kerning and ligature measurements.
    while (position < probeEnd) {
      const size_t nextPosition = nextCodepoint(position);
      if (renderer.getTextAdvanceX(fontId, text.substr(0, nextPosition).c_str(), EpdFontFamily::REGULAR) > maxWidth) {
        if (lastSpace > 0) return lastSpace;
        return position > 0 ? position : nextPosition;
      }
      rememberSpaces(position, nextPosition);
      position = nextPosition;
    }
  }
  return text.size();
}
}  // namespace

bool TxtReaderActivity::loadBook() {
  if (txt) return true;
  if (bookPath.empty()) {
    LOG_ERR("TRD", "Cannot load TXT with an empty path");
    return false;
  }
  auto loaded = ReaderActivity::loadTxt(bookPath);
  if (!loaded) return false;
  txt = std::move(loaded);
  return true;
}

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!loadBook()) {
    finish();
    return;
  }

  if (allowFastInitialRefresh_) {
    pagesUntilFullRefresh = std::max(SETTINGS.getRefreshFrequency(), 2);
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");
  READING_STATS.beginSession(txt->getPath(), txt->getTitle());

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);

  pendingPageTurn.store(0, std::memory_order_release);
  pendingPercentJump.store(-1, std::memory_order_release);

  if (txt && initialized) {
    if (!pageIndexComplete) savePartialPageIndexCache();
    saveProgress();
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  clearPageLayouts();
  if (txt) {
    READING_STATS.endSession();
    ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
    showPendingAchievementPopups(renderer);
  }
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  READING_STATS.noteActivity();
  const bool atEndOfBook = isAtEndOfBook();
  clearEndOfBookOptionsIfNeeded(atEndOfBook);
  if (handleEndOfBookMenu(atEndOfBook)) return;

  if (txt) {
    const int pageCount = displayedTotalPages();
    const int page = displayedPage();
    const int progress = pageCount > 0 ? std::min(99, (page + 1) * 100 / pageCount) : 0;
    READING_STATS.updateProgress(static_cast<uint8_t>(progress), false);
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int page = displayedPage();
    const int pageCount = displayedTotalPages();
    const int progressPercent = pageCount > 0 ? static_cast<int>((page + 1) * 100.0f / pageCount + 0.5f) : 0;
    startActivityForResult(std::make_unique<TxtReaderMenuActivity>(
                               renderer, mappedInput, txt ? txt->getTitle() : "", page + 1, pageCount,
                               progressPercent, SETTINGS.orientation),
                           [this](const ActivityResult& result) {
                             if (!std::holds_alternative<TxtMenuResult>(result.data)) {
                               LOG_ERR("TRD", "TXT reader menu returned an unexpected result");
                               return;
                             }
                             const auto& menu = std::get<TxtMenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<TxtReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(atEndOfBook, prevTriggered, nextTriggered)) return;

  // The render task exclusively owns page/index/preload state. Keep only one
  // intent while it is busy so a delayed display can never skip a page.
  pendingPageTurn.store(prevTriggered ? -1 : 1, std::memory_order_release);
  requestUpdate();
}

void TxtReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }

  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  initialized = false;
  pageIndexComplete = false;
  pendingResumePageTarget = -1;
  pendingPercentTarget = -1;
  pendingPageTurn.store(0, std::memory_order_release);
  pendingPercentJump.store(-1, std::memory_order_release);
  approximatePosition = false;
  approximatePageOffsets.clear();
  approximateLocalPage = 0;
  cancelNextPagePreparation();
  preparedPage = -1;
  preparedPageNextOffset = 0;
  preparedPageLines.clear();
  clearPageLayouts();
  removePartialPageIndexCache();
  pageOffsets.clear();
  currentPageLines.clear();
  requestUpdate();
}

void TxtReaderActivity::jumpToPercent(int percent) {
  percent = std::clamp(percent, 0, 100);
  pendingPercentJump.store(percent, std::memory_order_release);
  requestUpdate();
}

void TxtReaderActivity::applyPercentJump(const int percent) {
  if (!pageIndexComplete) {
    beginApproximatePosition(percent);
    LOG_DBG("TRS", "Approximate percent jump: %d%% offset=%u", percent,
            static_cast<unsigned>(displayedOffset()));
    requestUpdate();
    return;
  }

  if (totalPages <= 0) {
    return;
  }

  approximatePosition = false;
  approximatePageOffsets.clear();
  approximateLocalPage = 0;
  cancelNextPagePreparation();
  clearPageLayouts();
  int targetPage = percent >= 100 ? totalPages - 1 : (totalPages * percent) / 100;
  if (targetPage < 0) targetPage = 0;
  if (targetPage >= totalPages) targetPage = totalPages - 1;
  currentPage = targetPage;
  requestUpdate();
}

int TxtReaderActivity::displayPageKey() const {
  // Negative keys keep approximate pages distinct from the canonical index.
  return approximatePosition ? APPROXIMATE_PAGE_KEY_BASE - approximateLocalPage : currentPage;
}

int TxtReaderActivity::nextDisplayPageKey() const {
  return approximatePosition ? APPROXIMATE_PAGE_KEY_BASE - approximateLocalPage - 1 : currentPage + 1;
}

int TxtReaderActivity::displayedPage() const {
  return approximatePosition ? approximateBasePage + approximateLocalPage : currentPage;
}

int TxtReaderActivity::displayedTotalPages() const {
  return approximatePosition ? approximateTotalPages : totalPages;
}

size_t TxtReaderActivity::displayedOffset() const {
  if (approximatePosition && approximateLocalPage >= 0 &&
      approximateLocalPage < static_cast<int>(approximatePageOffsets.size())) {
    return approximatePageOffsets[approximateLocalPage];
  }
  return currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size()) ? pageOffsets[currentPage] : 0;
}

size_t TxtReaderActivity::alignApproximateOffset(size_t offset) const {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  if (fileSize == 0) return 0;
  offset = std::min(offset, fileSize - 1);

  constexpr size_t SEARCH_BYTES = 1024;
  const size_t start = offset > 3 ? offset - 3 : 0;
  const size_t length = std::min(SEARCH_BYTES + (offset - start), fileSize - start);
  std::vector<uint8_t> buffer(length);
  if (length == 0 || !txt->readContent(buffer.data(), start, length)) return offset;

  const size_t relative = offset - start;
  for (size_t i = relative; i < buffer.size(); ++i) {
    if (buffer[i] == '\n') return std::min(fileSize, start + i + 1);
  }

  size_t aligned = relative;
  while (aligned > 0 && (buffer[aligned] & 0xC0) == 0x80) --aligned;
  return start + aligned;
}

void TxtReaderActivity::beginApproximatePosition(const int percent, size_t offset) {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  if (fileSize == 0) return;
  if (offset == 0 && percent > 0) offset = fileSize * static_cast<size_t>(percent) / 100;
  offset = alignApproximateOffset(offset);

  const size_t indexedOffset = pageOffsets.empty() ? 0 : pageOffsets.back();
  const size_t indexedPages = pageOffsets.size();
  if (indexedOffset > 0 && indexedPages > 0) {
    approximateTotalPages = static_cast<int>(std::max<size_t>(
        totalPages, (fileSize * indexedPages + indexedOffset / 2) / indexedOffset));
  } else {
    approximateTotalPages = std::max(1, totalPages);
  }
  approximateBasePage = std::clamp(static_cast<int>(fileSize == 0 ? 0 :
                                                        (offset * static_cast<size_t>(approximateTotalPages)) / fileSize),
                                   0, std::max(0, approximateTotalPages - 1));
  approximatePosition = true;
  approximatePageOffsets.assign(1, offset);
  approximateLocalPage = 0;
  cancelNextPagePreparation();
  clearPageLayouts();
}

bool TxtReaderActivity::ensureApproximatePage(const int page) {
  if (!approximatePosition || page < 0) return false;
  while (page >= static_cast<int>(approximatePageOffsets.size())) {
    const size_t offset = approximatePageOffsets.back();
    const int sourcePage = static_cast<int>(approximatePageOffsets.size()) - 1;
    std::vector<std::string> lines;
    size_t nextOffset = offset;
    if (!loadPageAtOffset(offset, lines, nextOffset) || nextOffset <= offset) return false;
    cachePageLayout(APPROXIMATE_PAGE_KEY_BASE - sourcePage, nextOffset, lines);
    approximatePageOffsets.push_back(nextOffset);
  }
  return true;
}

void TxtReaderActivity::reconcileApproximatePosition() {
  if (!approximatePosition || pageOffsets.empty()) return;

  const size_t offset = displayedOffset();
  const auto it = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), offset);
  currentPage = it == pageOffsets.begin() ? 0 : static_cast<int>(std::distance(pageOffsets.begin(), it) - 1);
  approximatePosition = false;
  approximatePageOffsets.clear();
  approximateLocalPage = 0;
  cancelNextPagePreparation();
  clearPageLayouts();
  LOG_DBG("TRS", "Exact index ready; reconciled offset=%u to page=%d", static_cast<unsigned>(offset), currentPage);
}

void TxtReaderActivity::onReaderMenuConfirm(TxtReaderMenuActivity::MenuAction action) {
  switch (action) {
    case TxtReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int page = displayedPage();
      const int pageCount = displayedTotalPages();
      const int initialPercent = pageCount > 0 ? static_cast<int>((page + 1) * 100.0f / pageCount + 0.5f) : 0;
      startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled && std::holds_alternative<PercentResult>(result.data)) {
                                 jumpToPercent(std::get<PercentResult>(result.data).percent);
                               } else if (!result.isCancelled) {
                                 LOG_ERR("TRD", "Percent picker returned an unexpected result");
                               }
                             });
      break;
    }
    case TxtReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      requestUpdate();
      break;
    case TxtReaderMenuActivity::MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;
    case TxtReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      break;
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache() && !loadPartialPageIndexCache()) {
    pageOffsets = {0};
    pageIndexComplete = (txt->getFileSize() == 0);
  }

  // Load saved progress
  loadProgress();
  if (pendingApproximateResumeOffset > 0) {
    beginApproximatePosition(0, pendingApproximateResumeOffset);
    pendingApproximateResumeOffset = 0;
  } else if (!pageIndexComplete && currentPage >= static_cast<int>(pageOffsets.size())) {
    pendingResumePageTarget = currentPage;
    currentPage = std::max(0, static_cast<int>(pageOffsets.size()) - 1);
  }
  updateTotalPages();
  updateIndexProgress(false);

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  pageIndexComplete = true;
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::ensurePageIndexed(int page) {
  if (page < 0) {
    page = 0;
  }
  while (static_cast<int>(pageOffsets.size()) <= page && !pageIndexComplete) {
    if (!indexNextPage()) {
      break;
    }
  }
  updateTotalPages();
  return static_cast<int>(pageOffsets.size()) > page;
}

bool TxtReaderActivity::indexNextPage() {
  if (pageIndexComplete || pageOffsets.empty()) {
    return false;
  }

  std::vector<std::string> tempLines;
  size_t nextOffset = pageOffsets.back();
  if (!loadPageAtOffset(pageOffsets.back(), tempLines, nextOffset) || nextOffset <= pageOffsets.back()) {
    pageIndexComplete = true;
    savePageIndexCache();
    removePartialPageIndexCache();
    reconcileApproximatePosition();
    return false;
  }

  if (nextOffset >= txt->getFileSize()) {
    pageIndexComplete = true;
    savePageIndexCache();
    removePartialPageIndexCache();
  } else {
    pageOffsets.push_back(nextOffset);
  }
  updateTotalPages();
  if (pageIndexComplete) reconcileApproximatePosition();
  if (pageIndexComplete && pendingPercentTarget >= 0) {
    const int target = pendingPercentTarget;
    pendingPercentTarget = -1;
    jumpToPercent(target);
  }
  return true;
}

void TxtReaderActivity::buildPageIndexSlice() {
  if (pageIndexComplete) return;
  indexNextPage();
  updateIndexProgress(true);

  if (!pageIndexComplete && pageOffsets.size() >= lastCheckpointPageCount + INDEX_CHECKPOINT_PAGE_STEP) {
    savePartialPageIndexCache();
  }
  if (pendingResumePageTarget >= 0 && static_cast<int>(pageOffsets.size()) > pendingResumePageTarget) {
    currentPage = pendingResumePageTarget;
    pendingResumePageTarget = -1;
    requestUpdate();
  }
}

void TxtReaderActivity::prepareNextPage() {
  if (nextPagePrepareStage == NextPagePrepareStage::NONE) {
    nextPagePrepareStage = NextPagePrepareStage::LAYOUT;
    nextPagePrepareTarget = nextDisplayPageKey();
  }
  if (nextPagePrepareTarget != nextDisplayPageKey()) {
    cancelNextPagePreparation();
    return;
  }

  if (nextPagePrepareStage == NextPagePrepareStage::LAYOUT) {
    const unsigned long startMs = millis();
    size_t targetOffset = 0;
    if (approximatePosition) {
      if (!ensureApproximatePage(approximateLocalPage + 1)) {
        cancelNextPagePreparation();
        return;
      }
      targetOffset = approximatePageOffsets[approximateLocalPage + 1];
    } else if (nextPagePrepareTarget >= static_cast<int>(pageOffsets.size()) && !pageIndexComplete) {
      indexNextPage();
      updateIndexProgress(true);
    }
    if (!approximatePosition && nextPagePrepareTarget >= static_cast<int>(pageOffsets.size())) {
      cancelNextPagePreparation();
      return;
    }
    if (!approximatePosition) targetOffset = pageOffsets[nextPagePrepareTarget];

    if (const auto* cached = findPageLayout(nextPagePrepareTarget)) {
      nextPagePrepareLines = cached->lines;
      nextPagePrepareNextOffset = cached->nextOffset;
    } else if (!loadPageAtOffset(targetOffset, nextPagePrepareLines, nextPagePrepareNextOffset)) {
      cancelNextPagePreparation();
      return;
    } else {
      cachePageLayout(nextPagePrepareTarget, nextPagePrepareNextOffset, nextPagePrepareLines);
    }

    LOG_DBG("TRS", "Prepared page %d layout=%lums lines=%u", nextPagePrepareTarget, millis() - startMs,
            static_cast<unsigned>(nextPagePrepareLines.size()));
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
      cache->resetStats();
    }
    nextPagePrepareStage = NextPagePrepareStage::FONT;
    requestUpdate();
    return;
  }

  // SdCardFont replaces its page glyph table on every prewarm call, so this
  // must remain one full-page call rather than a line-by-line accumulation.
  // Layout already ran in the preceding update, keeping this phase bounded to
  // the bitmap reads required for the actual next page.
  const unsigned long startMs = millis();
  if (auto* cache = renderer.getFontCacheManager()) {
    std::string pageText;
    for (const auto& line : nextPagePrepareLines) {
      pageText += line;
      pageText.push_back('\n');
    }
    cache->prewarmCache(cachedFontId, pageText.c_str(), 0x01);
    LOG_DBG("TRS", "Prepared page %d font=%lums", nextPagePrepareTarget, millis() - startMs);
    cache->logStats("TXT-next");
  }

  preparedPageLines = std::move(nextPagePrepareLines);
  preparedPageNextOffset = nextPagePrepareNextOffset;
  preparedPage = nextPagePrepareTarget;
  LOG_DBG("TRS", "Prepared page %d font ready", preparedPage);
  nextPagePrepareStage = NextPagePrepareStage::NONE;
  nextPagePrepareTarget = -1;
  scheduleBackgroundWork();
}

void TxtReaderActivity::cancelNextPagePreparation() {
  nextPagePrepareRequested = false;
  nextPagePrepareStage = NextPagePrepareStage::NONE;
  nextPagePrepareTarget = -1;
  nextPagePrepareNextOffset = 0;
  nextPagePrepareLines.clear();
}

bool TxtReaderActivity::applyPendingPageTurn() {
  const int8_t direction = pendingPageTurn.exchange(0, std::memory_order_acq_rel);
  if (direction == 0) return false;

  // All of these fields are render-task owned. Cancel only here, never from
  // the input task, so a layout cannot observe a half-cleared target page.
  indexBuildOnlyRequested = false;
  cancelNextPagePreparation();

  if (approximatePosition) {
    if (direction < 0) {
      if (approximateLocalPage > 0) --approximateLocalPage;
    } else if (ensureApproximatePage(approximateLocalPage + 1)) {
      ++approximateLocalPage;
    }
    return true;
  }

  if (direction < 0) {
    if (currentPage > 0) --currentPage;
    return true;
  }

  if (currentPage + 1 >= static_cast<int>(pageOffsets.size()) && !pageIndexComplete) {
    lastIndexBuildTick = millis() - INDEX_BUILD_TICK_MS;
    indexNextPage();
    updateIndexProgress(true);
  }
  updateTotalPages();
  if (currentPage < static_cast<int>(pageOffsets.size()) - 1) {
    ++currentPage;
  } else if (pageIndexComplete) {
    // Keep a one-step sentinel after the last page so the shared reader layer
    // can display the end-of-book menu without clamping back to the page.
    currentPage = totalPages;
  }
  return true;
}

void TxtReaderActivity::scheduleBackgroundWork() {
  if (!initialized || pendingPageTurn.load(std::memory_order_acquire) != 0) return;

  const int nextPage = nextDisplayPageKey();
  const bool canPrepareNext = preparedPage != nextPage &&
                              (approximatePosition || nextPage < static_cast<int>(pageOffsets.size()) || !pageIndexComplete);
  if (canPrepareNext && nextPagePrepareStage == NextPagePrepareStage::NONE && !nextPagePrepareRequested) {
    nextPagePrepareRequested = true;
    requestUpdate();
    return;
  }

  if (!pageIndexComplete) {
    indexBuildOnlyRequested = true;
    requestUpdate();
  }
}

const TxtReaderActivity::PageLayout* TxtReaderActivity::findPageLayout(const int page) const {
  for (const auto& cached : pageLayoutCache) {
    if (cached.page == page) return &cached;
  }
  return nullptr;
}

void TxtReaderActivity::cachePageLayout(const int page, const size_t nextOffset, const std::vector<std::string>& lines) {
  for (auto& cached : pageLayoutCache) {
    if (cached.page == page) {
      cached.nextOffset = nextOffset;
      cached.lines = lines;
      return;
    }
  }
  pageLayoutCache.push_back({page, nextOffset, lines});
  while (pageLayoutCache.size() > PAGE_LAYOUT_CACHE_SIZE) {
    size_t evict = 0;
    int greatestDistance = -1;
    for (size_t i = 0; i < pageLayoutCache.size(); ++i) {
      const int distance = pageLayoutCache[i].page > currentPage ? pageLayoutCache[i].page - currentPage
                                                                   : currentPage - pageLayoutCache[i].page;
      if (distance > greatestDistance) {
        greatestDistance = distance;
        evict = i;
      }
    }
    pageLayoutCache.erase(pageLayoutCache.begin() + evict);
  }
}

void TxtReaderActivity::clearPageLayouts() { pageLayoutCache.clear(); }

void TxtReaderActivity::updateIndexProgress(const bool requestRefresh) {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  const size_t indexedOffset = pageIndexComplete ? fileSize : (pageOffsets.empty() ? 0 : pageOffsets.back());
  const uint8_t rawProgress = fileSize == 0 ? 100 : static_cast<uint8_t>(std::min<size_t>(100, indexedOffset * 100 / fileSize));
  const uint8_t progress = rawProgress >= 100 ? 100 : static_cast<uint8_t>((rawProgress / INDEX_PROGRESS_STEP) * INDEX_PROGRESS_STEP);
  indexProgressPercent = progress;

  if (!requestRefresh ||
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::PRELOAD_PROGRESS) {
    return;
  }
  const bool advanced = lastDisplayedIndexProgressPercent == 255 || progress > lastDisplayedIndexProgressPercent;
  const bool due = millis() - lastIndexProgressRefreshMs >= INDEX_PROGRESS_REFRESH_MS;
  if (advanced && (progress == 100 || due)) {
    lastDisplayedIndexProgressPercent = progress;
    lastIndexProgressRefreshMs = millis();
    indexProgressRefreshPending = true;
  }
}

void TxtReaderActivity::updateTotalPages() {
  totalPages = static_cast<int>(pageOffsets.size());
  if (!pageIndexComplete) {
    totalPages++;
  }
  if (totalPages < 1) {
    totalPages = 1;
  }
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Begin with the amount a normal page needs. A long paragraph can request a
  // larger retry below, but short pages never pay to inspect an unrelated 4 KiB.
  size_t chunkSize = std::min(INITIAL_LAYOUT_CHUNK_SIZE, fileSize - offset);
  const size_t maxChunkSize = std::min(MAX_LAYOUT_CHUNK_SIZE, fileSize - offset);
  while (true) {
    outLines.clear();
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      // This scans only until the first visual-line boundary. Avoid measuring
      // the whole remaining paragraph before every wrapped line.
      const size_t breakPos = fittingPrefix(renderer, cachedFontId, line, viewportWidth);
      if (breakPos >= line.size()) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + ((lineEnd < chunkSize && buffer[lineEnd] == '\n') ? 1 : 0);
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

    // Do not make a page break simply because the initial read ended in the
    // middle of a source line. Retry with a little more text only when this
    // attempt did not fill the visual page and the file still has content.
    if (outLines.size() < static_cast<size_t>(linesPerPage) && nextOffset < fileSize && chunkSize < maxChunkSize) {
      chunkSize = std::min(maxChunkSize, chunkSize + LAYOUT_CHUNK_GROWTH);
      continue;
    }

    return !outLines.empty();
  }
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  const int16_t requestedPercent = pendingPercentJump.exchange(-1, std::memory_order_acq_rel);
  const bool percentJumpApplied = requestedPercent >= 0;
  const bool pageTurnApplied = percentJumpApplied ? false : applyPendingPageTurn();
  if (percentJumpApplied) {
    indexBuildOnlyRequested = false;
    applyPercentJump(requestedPercent);
  }

  if (isAtEndOfBook()) {
    READING_STATS.updateProgress(100, true);
    renderEndOfBook(mappedInput);
    return;
  }

  if (!pageTurnApplied && !percentJumpApplied &&
      (nextPagePrepareRequested || nextPagePrepareStage != NextPagePrepareStage::NONE)) {
    nextPagePrepareRequested = false;
    prepareNextPage();
    return;
  }

  const bool buildOnly = indexBuildOnlyRequested;
  indexBuildOnlyRequested = false;

  // Page offsets are shared with input handling. Build one slice only while
  // rendering owns the lock, so page turns never race a vector reallocation.
  if (!pageIndexComplete && buildOnly && millis() - lastIndexBuildTick >= INDEX_BUILD_TICK_MS) {
    lastIndexBuildTick = millis();
    buildPageIndexSlice();
  }

  // Normal background slices should not redraw the whole e-paper page. A
  // redraw is reserved for the 5% progress checkpoints or a user action.
  if (buildOnly && !indexProgressRefreshPending) {
    scheduleBackgroundWork();
    return;
  }
  indexProgressRefreshPending = false;

  if (!approximatePosition) ensurePageIndexed(currentPage);

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check for the canonical, from-start page index.
  if (!approximatePosition) {
    if (currentPage < 0) currentPage = 0;
    if (currentPage >= totalPages) currentPage = totalPages - 1;
  }

  // Load current page content
  const int pageKey = displayPageKey();
  size_t offset = displayedOffset();
  size_t nextOffset = offset;
  const bool usePreparedPage = preparedPage == pageKey;
  const unsigned long layoutStartMs = millis();
  if (usePreparedPage) {
    currentPageLines = preparedPageLines;
    nextOffset = preparedPageNextOffset;
  } else if (const auto* cached = findPageLayout(pageKey)) {
    currentPageLines = cached->lines;
    nextOffset = cached->nextOffset;
  } else {
    currentPageLines.clear();
    loadPageAtOffset(offset, currentPageLines, nextOffset);
    if (!currentPageLines.empty()) cachePageLayout(pageKey, nextOffset, currentPageLines);
  }
  const unsigned long layoutMs = millis() - layoutStartMs;
  if (!usePreparedPage && layoutMs >= 50) {
    LOG_DBG("TRS", "Page %d layout=%lums lines=%u", displayedPage(), layoutMs,
            static_cast<unsigned>(currentPageLines.size()));
  }
  if (!approximatePosition && !pageIndexComplete && currentPage + 1 == static_cast<int>(pageOffsets.size())) {
    if (nextOffset > offset && nextOffset < txt->getFileSize()) {
      pageOffsets.push_back(nextOffset);
      updateIndexProgress(false);
    } else if (nextOffset >= txt->getFileSize()) {
      pageIndexComplete = true;
      savePageIndexCache();
      removePartialPageIndexCache();
      updateIndexProgress(false);
    }
    updateTotalPages();
  }

  renderer.clearScreen();
  renderPage(usePreparedPage);
  preparedPage = -1;
  preparedPageNextOffset = 0;
  preparedPageLines.clear();

  if (!pageIndexComplete) savePartialPageIndexCache();
  const int page = displayedPage();
  const int pageCount = displayedTotalPages();
  const bool completed = nextOffset >= txt->getFileSize();
  const int progress = completed ? 100 : (pageCount > 0 ? std::min(99, (page + 1) * 100 / pageCount) : 0);
  READING_STATS.updateProgress(static_cast<uint8_t>(progress), completed, {}, static_cast<uint8_t>(progress));
  saveProgress();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  scheduleBackgroundWork();
}

void TxtReaderActivity::renderPage(const bool fontPrewarmed) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  const auto drawAndDisplay = [&]() {
    renderLines();
    renderStatusBar();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    if (SETTINGS.textAntiAliasing) {
      ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
    }
  };

  if (fontPrewarmed) {
    LOG_DBG("TRS", "Page %d uses prepared font cache", displayedPage());
    drawAndDisplay();
    return;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  auto* fcm = renderer.getFontCacheManager();
  const unsigned long prewarmStartMs = millis();
  auto scope = fcm->createPrewarmScope();
  renderLines();
  scope.endScanAndPrewarm();
  const unsigned long prewarmMs = millis() - prewarmStartMs;
  if (prewarmMs >= 50) {
    LOG_DBG("TRS", "Page %d font prewarm=%lums", displayedPage(), prewarmMs);
    fcm->logStats("TXT-render");
  }
  drawAndDisplay();
  // scope destructor clears font cache via FontCacheManager.
}

void TxtReaderActivity::renderStatusBar() const {
  const int page = displayedPage();
  const int pageCount = displayedTotalPages();
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  const float progress = approximatePosition && fileSize > 0
                             ? displayedOffset() * 100.0f / fileSize
                             : (pageCount > 0 ? (page + 1) * 100.0f / pageCount : 0);
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  const int indexProgress = pageIndexComplete ? 100 : indexProgressPercent;
  GUI.drawStatusBar(renderer, progress, page + 1, pageCount, title, 0, 0, true, false, indexProgress);
}

void TxtReaderActivity::saveProgress() {
  uint8_t data[12] = {};
  const uint32_t page = static_cast<uint32_t>(std::max(0, displayedPage()));
  const uint32_t offset = static_cast<uint32_t>(displayedOffset());
  memcpy(data, &PROGRESS_MAGIC, sizeof(PROGRESS_MAGIC));
  memcpy(data + 4, &page, sizeof(page));
  memcpy(data + 8, &offset, sizeof(offset));
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  } else {
    LOG_DBG("TRS", "Saved progress: page=%u offset=%u", static_cast<unsigned>(page), static_cast<unsigned>(offset));
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[12] = {};
    const size_t bytesRead = f.read(data, sizeof(data));
    uint32_t magic = 0;
    memcpy(&magic, data, sizeof(magic));
    if (bytesRead == sizeof(data) && magic == PROGRESS_MAGIC) {
      uint32_t savedPage = 0;
      uint32_t savedOffset = 0;
      memcpy(&savedPage, data + 4, sizeof(savedPage));
      memcpy(&savedOffset, data + 8, sizeof(savedOffset));
      currentPage = static_cast<int>(savedPage);
      const auto offsetIt = std::lower_bound(pageOffsets.begin(), pageOffsets.end(), savedOffset);
      if (offsetIt != pageOffsets.end() && *offsetIt == savedOffset) {
        currentPage = static_cast<int>(std::distance(pageOffsets.begin(), offsetIt));
      } else if (savedOffset > 0 && savedOffset < txt->getFileSize()) {
        pendingApproximateResumeOffset = savedOffset;
      }
    } else if (bytesRead >= 4) {
      currentPage = data[0] + (data[1] << 8);
    } else {
      return;
    }
    if (pageIndexComplete && currentPage >= totalPages) {
      currentPage = totalPages - 1;
    }
    if (currentPage < 0) currentPage = 0;
    LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
  }
}

bool TxtReaderActivity::isAtEndOfBook() const {
  return txt && pageIndexComplete && totalPages > 0 && currentPage >= totalPages;
}

void TxtReaderActivity::onReturnFromEndOfBook() {
  if (!isAtEndOfBook()) return;
  currentPage = std::max(0, totalPages - 1);
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  pageIndexComplete = true;
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

bool TxtReaderActivity::loadPartialPageIndexCache() {
  HalFile f;
  const std::string path = txt->getCachePath() + "/index.building";
  if (!Storage.openFileForRead("TRS", path, f)) return false;

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t width = 0, lines = 0, fontId = 0, margin = 0;
  uint8_t alignment = 0;
  uint32_t numPages = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, fileSize);
  serialization::readPod(f, width);
  serialization::readPod(f, lines);
  serialization::readPod(f, fontId);
  serialization::readPod(f, margin);
  serialization::readPod(f, alignment);
  serialization::readPod(f, numPages);
  const uint64_t expectedSize = 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1 + 4 + static_cast<uint64_t>(numPages) * 4;
  if (magic != PARTIAL_CACHE_MAGIC || version != CACHE_VERSION || fileSize != txt->getFileSize() ||
      width != viewportWidth || lines != linesPerPage || fontId != cachedFontId || margin != cachedScreenMargin ||
      alignment != cachedParagraphAlignment || numPages == 0 || f.fileSize64() != expectedSize) {
    return false;
  }

  pageOffsets.clear();
  pageOffsets.reserve(numPages);
  for (uint32_t i = 0; i < numPages; ++i) {
    uint32_t offset = 0;
    serialization::readPod(f, offset);
    if (offset >= txt->getFileSize() || (!pageOffsets.empty() && offset <= pageOffsets.back())) {
      pageOffsets.clear();
      return false;
    }
    pageOffsets.push_back(offset);
  }
  pageIndexComplete = false;
  lastCheckpointPageCount = pageOffsets.size();
  LOG_DBG("TRS", "Resumed partial page index: %u pages", static_cast<unsigned>(pageOffsets.size()));
  return true;
}

bool TxtReaderActivity::savePartialPageIndexCache() {
  if (pageIndexComplete || pageOffsets.empty()) return true;
  const std::string finalPath = txt->getCachePath() + "/index.building";
  const std::string tmpPath = finalPath + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("TRS", tmpPath, f)) return false;
    serialization::writePod(f, PARTIAL_CACHE_MAGIC);
    serialization::writePod(f, CACHE_VERSION);
    serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
    serialization::writePod(f, static_cast<int32_t>(viewportWidth));
    serialization::writePod(f, static_cast<int32_t>(linesPerPage));
    serialization::writePod(f, static_cast<int32_t>(cachedFontId));
    serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
    serialization::writePod(f, cachedParagraphAlignment);
    serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));
    for (const size_t offset : pageOffsets) serialization::writePod(f, static_cast<uint32_t>(offset));
    f.flush();
  }
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) return false;
  lastCheckpointPageCount = pageOffsets.size();
  return true;
}

void TxtReaderActivity::removePartialPageIndexCache() const {
  if (txt) Storage.remove((txt->getCachePath() + "/index.building").c_str());
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
