#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "AchievementsStore.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "WeReadStore.h"
#include "activities/apps/weread/webapi/WeReadProgressSyncActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/BookmarkFile.h"
#include "util/AchievementPopupUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;
constexpr unsigned long INCREMENTAL_BUILD_TICK_MS = 80UL;
constexpr uint8_t PRELOAD_PROGRESS_STEP = 10;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  READING_STATS.updateBookPath(srcPath, dstPath);
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  currentPageVisibleOffset.reset();
  pendingOffsetJump.reset();

  // Failed image assets are suppressed for one reader session so repeated
  // grayscale passes cannot keep retrying a broken decoder. A new EPUB open
  // starts a fresh retry window.
  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* srcPath, const char* destPath) {
    return static_cast<Epub*>(ctx)->extractItemToFile(srcPath, destPath);
  });

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();
  WeReadStore::findBookIdForPath(epub->getPath(), wereadBookId_, sizeof(wereadBookId_));
  bool hasSavedProgress = false;

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10] = {};
    int dataSize = f.read(data, sizeof(data));
    // Current progress records may include the visible text offset (10 bytes).
    // The first six bytes are still the same spine/page/page-count payload, so
    // parse them for all supported record sizes. Previously a 10-byte record
    // was recognized only for its offset, leaving the resume chapter/page at
    // their defaults.
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      hasSavedProgress = true;
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6 || dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    if (dataSize == 10) {
      hasSavedProgress = true;
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                  (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // A freshly generated WeRead EPUB carries the remote percentage in a small
  // sidecar until the reader has written its first local progress record. Do
  // not let that sidecar override an existing local resume position.
  if (wereadBookId_[0]) {
    float initialProgress = 0.0f;
    const bool loaded = WeReadStore::loadInitialProgress(wereadBookId_, initialProgress);
    if (hasSavedProgress || !loaded || initialProgress <= 0.0f) {
      WeReadStore::clearInitialProgress(wereadBookId_);
    } else if (jumpToFraction(initialProgress)) {
      clearInitialProgressAfterSave_ = true;
    } else {
      WeReadStore::clearInitialProgress(wereadBookId_);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  READING_STATS.beginSession(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);

  // ImageBlock keeps a process-wide extractor hook for lazy EPUB assets. Clear
  // it before releasing this activity's Epub so no later render can call a
  // dangling context pointer.
  ImageBlock::setExtractor(nullptr, nullptr);
  ImageBlock::releaseRenderCache();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  if (epub) {
    // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist
    // the pre-footnote position so the book reopens at the link origin, not the
    // footnote.
    if (footnoteDepth > 0) {
      const SavedPosition& origin = savedPositions[0];
      if (!saveProgress(origin.spineIndex, origin.pageNumber, 0)) {
        LOG_ERR("ERS", "Failed to save progress before leaving footnote");
      }
    } else if (section && section->pageCount > 0 &&
               section->currentPage >= 0 && section->currentPage < section->pageCount) {
      // A page turn changes section->currentPage before the render task writes
      // progress. Save the in-memory page here as well, so a quick Back or
      // sleep cannot discard the latest completed navigation.
      if (!saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
        LOG_ERR("ERS", "Failed to save progress before leaving reader");
      }
    } else if (!section && currentSpineIndex >= 0 && currentSpineIndex < epub->getSpineItemsCount() &&
               !pendingPageJump.has_value()) {
      // Crossing into the next chapter releases the old Section immediately.
      // Preserve that transition if the user exits before the new chapter is
      // rendered; do not persist the UINT16_MAX previous-chapter sentinel.
      if (!saveProgress(currentSpineIndex, nextPageNumber, 0)) {
        LOG_ERR("ERS", "Failed to save chapter transition before leaving reader");
      }
    }
  }

  if (epub) {
    READING_STATS.endSession();
    ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
    showPendingAchievementPopups(renderer);
  }

  section.reset();
  clearNextChapterPreload();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  if (!epub) {
    requestUpdate();
    return;
  }

  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                         [this](const ActivityResult& result) {
                           // Always apply orientation change even if the menu was cancelled.
                           const auto& menu = std::get<MenuResult>(result.data);
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  READING_STATS.noteActivity();

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  clearEndOfBookOptionsIfNeeded(atEndOfBook);
  if (handleEndOfBookMenu(atEndOfBook, ignoreNextConfirmRelease)) return;

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Schedule one parser slice from the main loop. The render task must not
  // chain slices directly, otherwise it starves input polling.
  if (((section && section->isBuilding()) || (nextChapterPreload && nextChapterPreload->isBuilding())) &&
      !RenderLock::peek() && !mappedInput.wasAnyPressed() &&
      !mappedInput.wasAnyReleased() && millis() - lastIncrementalBuildTick >= INCREMENTAL_BUILD_TICK_MS) {
    lastIncrementalBuildTick = millis();
    requestUpdate();
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches WeRead for WeRead-generated EPUBs, otherwise KOReader.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if ((wereadBookId_[0] && launchWeReadSync()) || launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(atEndOfBook, prevTriggered, nextTriggered)) return;

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    {
      RenderLock lock(*this);
      clearDeferredReposition();
      if (!nextTriggered && section && section->currentPage > 0) {
        section->currentPage = 0;
        pageRenderRequested = true;
      } else {
        nextPageNumber = 0;
        if (nextTriggered) {
          currentSpineIndex++;
        } else if (currentSpineIndex > 0) {
          currentSpineIndex--;
        }
        section.reset();
        pageRenderRequested = true;
      }
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
bool EpubReaderActivity::jumpToFraction(const float fraction) {
  if (!epub || !std::isfinite(fraction)) {
    return false;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return false;
  }

  const float clampedFraction = std::clamp(fraction, 0.0f, 1.0f);
  const size_t targetSize = clampedFraction >= 1.0f
                                ? bookSize - 1
                                : static_cast<size_t>(static_cast<double>(bookSize) * clampedFraction);
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return false;
  }

  int targetSpineIndex = spineCount - 1;
  size_t previousCumulative = 0;
  for (int i = 0; i < spineCount; ++i) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      previousCumulative = i > 0 ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = cumulative > previousCumulative ? cumulative - previousCumulative : 0;
  pendingSpineProgress = spineSize == 0
                             ? 0.0f
                             : static_cast<float>(targetSize - previousCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
  return true;
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
      return;
    }

    const auto& sync = std::get<ProgressChangeResult>(result.data);

    // A bookmark's visible-text offset identifies the same content even when
    // changed fonts, margins or orientation produce a different page grid.
    if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      clearDeferredReposition();
      if (section && currentSpineIndex == sync.spineIndex) {
        const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
        if (page.has_value()) {
          section->currentPage = *page;
          currentPageVisibleOffset = sync.visibleTextOffset;
          pageRenderRequested = true;
        } else {
          // The requested offset may be beyond a partial cache's current
          // watermark. Force the normal section-load path so the background
          // builder can reach it, rather than falling back permanently to the
          // stale page number.
          cachedSpineIndex = sync.spineIndex;
          cachedChapterTotalPageCount = sync.totalPages;
          cachedVisibleTextOffset = sync.visibleTextOffset;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          pageRenderRequested = true;
          section.reset();
        }
      } else {
        currentSpineIndex = sync.spineIndex;
        cachedSpineIndex = sync.spineIndex;
        cachedVisibleTextOffset = sync.visibleTextOffset;
        pendingOffsetJump = sync.visibleTextOffset;
        cachedChapterTotalPageCount = sync.totalPages;
        nextPageNumber = std::max(0, sync.page);
        currentPageVisibleOffset.reset();
        pageRenderRequested = true;
        section.reset();
      }
      requestUpdate();
      return;
    }

    int targetSpineIndex = sync.spineIndex;
    int targetPage = sync.page;
    const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
    const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                sync.page < sync.totalPages && activeTotalPages == sync.totalPages;
    if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
      const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
      const CrossPointPosition fallback =
          ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
      targetSpineIndex = fallback.spineIndex;
      targetPage = fallback.pageNumber;
    }

    RenderLock lock(*this);
    clearDeferredReposition();
    if (currentSpineIndex != targetSpineIndex) {
      currentSpineIndex = targetSpineIndex;
      nextPageNumber = std::max(0, targetPage);
      section.reset();
    } else if (section && section->currentPage != targetPage) {
      section->currentPage = std::max(0, targetPage);
    } else if (!section) {
      nextPageNumber = std::max(0, targetPage);
    }
    requestUpdate();
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              clearDeferredReposition();
              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->estimatedTotalPages();
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (!launchWeReadSync()) {
        launchKOReaderSync();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchWeReadSync() {
  if (!epub || !wereadBookId_[0]) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  const float localFraction = totalPages > 1
                                  ? std::clamp(static_cast<float>(currentPage) / static_cast<float>(totalPages - 1),
                                               0.0f, 1.0f)
                                  : 0.0f;
  const CrossPointPosition localPosition = getCurrentPosition();
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  WeReadProgressContext context =
      WeReadProgressSyncActivity::makeContext(*epub, wereadBookId_, localFraction, localPosition);
  auto sync = makeUniqueNoThrow<WeReadProgressSyncActivity>(renderer, mappedInput, savedEpubPath, wereadBookId_,
                                                            context);
  if (!sync) {
    LOG_ERR("WRSync", "OOM: WeReadProgressSyncActivity");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("WRSync", "Releasing epub for progress sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) nextPageNumber = section->currentPage;
    section.reset();
    epub.reset();
  }
  activityManager.replaceActivity(std::move(sync));
  return true;
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    currentPageVisibleOffset.reset();
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
    }
    currentPageVisibleOffset.reset();
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  RenderLock lock(false);
  if (!lock.ownsLock()) {
    constexpr int8_t MAX_QUEUED_PAGE_TURNS = 3;
    int8_t queued = queuedPageTurns.load();
    int8_t updated;
    do {
      updated = isForwardTurn ? std::min<int8_t>(MAX_QUEUED_PAGE_TURNS, static_cast<int8_t>(queued + 1))
                              : std::max<int8_t>(-MAX_QUEUED_PAGE_TURNS, static_cast<int8_t>(queued - 1));
    } while (!queuedPageTurns.compare_exchange_weak(queued, updated));
    requestUpdate();
    return;
  }

  applyPageTurnLocked(isForwardTurn);

  lastPageTurnTime = millis();
  requestUpdate();
}

void EpubReaderActivity::applyPageTurnLocked(const bool isForwardTurn) {
  clearDeferredReposition();

  if (!section) {
    return;
  }

  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
      pageRenderRequested = true;
    } else if (section->isBuilding()) {
      // Keep the user's forward turn pending until the incremental index has
      // produced the next page. Do not treat a partial cache as chapter end.
      pendingForwardPageTurn = true;
      lastIncrementalBuildTick = millis() - INCREMENTAL_BUILD_TICK_MS;
    } else {
      nextPageNumber = 0;
      clearNextChapterPreload();
      currentSpineIndex++;
      section.reset();
      pageRenderRequested = true;
    }
  } else if (section->currentPage > 0) {
    section->currentPage--;
    pageRenderRequested = true;
  } else if (currentSpineIndex > 0) {
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
    clearNextChapterPreload();
    currentSpineIndex--;
    section.reset();
    pageRenderRequested = true;
  }
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // A corrupt/invalid EPUB can leave the indexing popup on screen forever if
  // the section build fails. Surface a clear error and stop auto paging.
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    READING_STATS.updateProgress(100, true);
    renderEndOfBook(mappedInput);
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  int8_t queuedTurns = queuedPageTurns.load();
  if (queuedTurns != 0 && section) {
    const bool isForwardTurn = queuedTurns > 0;
    const int8_t remaining = static_cast<int8_t>(queuedTurns + (isForwardTurn ? -1 : 1));
    if (!queuedPageTurns.compare_exchange_strong(queuedTurns, remaining)) {
      // A newer key event won the race. Leave it for the next render tick.
      return;
    }
    applyPageTurnLocked(isForwardTurn);
    lastPageTurnTime = millis();
  }

  if (section && section->isBuilding()) {
    const auto buildResult = section->buildNextChunk(1);
    if (buildResult == Section::BuildResult::Failed) {
      LOG_ERR("ERS", "Incremental section build failed");
      section.reset();
      showBuildError();
      return;
    }
    if (buildResult == Section::BuildResult::PausedLowMemory) {
      // Keep the preview or last completed page visible. The main-loop
      // scheduler will retry after transient page/image allocations are freed.
      return;
    }

    // A visible-text target can become resolvable before the whole chapter is
    // indexed. Rebase as soon as the active build reaches it; this avoids
    // waiting for a large chapter to finish before a bookmark/sync jump lands.
    applyCachedVisibleTextOffset();

    if (buildResult == Section::BuildResult::Complete) {
      // Anchor and percentage navigation need the completed page table for an
      // exact result. Build it incrementally, then reposition without ever
      // falling back to a blocking createSectionFile() call.
      if (!pendingAnchor.empty()) {
        if (const auto page = section->getPageForAnchor(pendingAnchor)) {
          section->currentPage = *page;
          LOG_DBG("ERS", "Resolved deferred anchor '%s' to page %d", pendingAnchor.c_str(), *page);
        } else {
          LOG_DBG("ERS", "Deferred anchor '%s' not found", pendingAnchor.c_str());
        }
        pendingAnchor.clear();
        pageRenderRequested = true;
      }
      if (pendingPercentJump && section->pageCount > 0) {
        int targetPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
        section->currentPage = std::min(targetPage, static_cast<int>(section->pageCount) - 1);
        pendingPercentJump = false;
        pageRenderRequested = true;
      }
      applyCachedVisibleTextOffset();
    }

    if (pendingResumePageTarget && section->hasBuiltPage(*pendingResumePageTarget)) {
      section->currentPage = *pendingResumePageTarget;
      pendingResumePageTarget.reset();
      pageRenderRequested = true;
    }

    if (pendingForwardPageTurn && section->currentPage < section->pageCount - 1) {
      section->currentPage++;
      pendingForwardPageTurn = false;
      pageRenderRequested = true;
    }

    // Keep the already rendered page on screen until its replacement has
    // been produced. This render tick only advances the parser.
    if (!section->hasBuiltPage(section->currentPage) || !pageRenderRequested) {
      return;
    }
  }

  // Keep next-chapter work off the critical rendering path. A scheduled idle
  // tick builds one parser chunk and returns without redrawing the current page.
  if (!pageRenderRequested && nextChapterPreload && nextChapterPreload->isBuilding()) {
    advanceNextChapterPreload();
    return;
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    currentPageVisibleOffset.reset();
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const bool sectionCacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
        SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
    if (!sectionCacheLoaded || section->isPartial()) {
      LOG_DBG("ERS", section->isPartial() ? "Partial section cache found, extending..." : "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));
      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
      const auto preloadProgressFn = [this](const uint8_t progress) { updatePreloadProgress(progress, true); };
      const bool loadedPartial = sectionCacheLoaded && section->isPartial();

      if (loadedPartial) {
        // A partial cache is immediately readable. Start a fresh parser from
        // the beginning so it can extend the committed prefix in the
        // background; loadPageFromSectionFile() falls back to the old file
        // until the new parser reaches the requested page.
        if (section->beginIncrementalBuild(
                SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, preloadProgressFn)) {
          const int requestedPage = pendingPageJump.has_value() ? static_cast<int>(*pendingPageJump) : nextPageNumber;
          pendingPageJump.reset();
          if (requestedPage >= section->pageCount) {
            pendingResumePageTarget = static_cast<uint16_t>(std::max(0, requestedPage));
            section->currentPage = std::max(0, static_cast<int>(section->pageCount) - 1);
          } else {
            section->currentPage = std::max(0, requestedPage);
          }
          beginPreloadProgress();
          pageRenderRequested = true;
          lastIncrementalBuildTick = millis() - INCREMENTAL_BUILD_TICK_MS;
          return;
        }
        LOG_ERR("ERS", "Failed to resume partial section build; showing cached prefix");
      }

      const bool canPreviewBeforeFullBuild = pendingAnchor.empty() && !pendingPercentJump;
      const int previewPageNumber = pendingPageJump.has_value() ? static_cast<int>(*pendingPageJump) : nextPageNumber;

      // A prior exit may have left a valid page cache prefix. Do not parse the
      // chapter synchronously again just to preview the saved page: that path
      // recreates the largest temporary ParsedText allocation and can abort on
      // a large CJK paragraph. Resume the prefix and let normal slices reach
      // the requested page instead.
      if (!loadedPartial && canPreviewBeforeFullBuild && previewPageNumber >= 0 && section->hasIncrementalBuildCheckpoint() &&
          section->beginIncrementalBuild(
              SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
              SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, preloadProgressFn)) {
        const uint16_t requestedPage = static_cast<uint16_t>(previewPageNumber);
        pendingPageJump.reset();
        beginPreloadProgress();
        lastIncrementalBuildTick = millis() - INCREMENTAL_BUILD_TICK_MS;

        if (section->hasBuiltPage(requestedPage)) {
          section->currentPage = requestedPage;
          pageRenderRequested = true;
        } else {
          pendingResumePageTarget = requestedPage;
          // Render the last confirmed page while the parser catches up.
          section->currentPage = std::max(0, static_cast<int>(section->pageCount) - 1);
          pageRenderRequested = true;
        }
        return;
      }

      if (!loadedPartial && canPreviewBeforeFullBuild && previewPageNumber >= 0) {
        const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
        auto previewPage = section->loadPageDuringBuild(renderSpec, static_cast<uint16_t>(previewPageNumber));
        if (previewPage) {
          section->currentPage = previewPageNumber;
          if (cachedChapterTotalPageCount > 0) {
            section->pageCount = cachedChapterTotalPageCount;
          }
          pendingPageJump.reset();
          currentPageFootnotes = std::move(previewPage->footnotes);

          renderer.clearScreen();
          const auto start = millis();
          renderContents(std::move(previewPage), orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                         orientedMarginLeft);
          LOG_DBG("ERS", "Rendered preview page in %dms", millis() - start);

          // Keep the preview on screen and build the remaining cache in small
          // slices. This also applies to resumed positions and chapter jumps.
          if (section->beginIncrementalBuild(
                  SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                  SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, preloadProgressFn)) {
            beginPreloadProgress();
            pageRenderRequested = false;
            lastIncrementalBuildTick = millis() - INCREMENTAL_BUILD_TICK_MS;
            return;
          }

          beginPreloadProgress();
          if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                          SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                          viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                          SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, nullptr,
                                          preloadProgressFn)) {
            LOG_ERR("ERS", "Failed to persist page data to SD after preview");
            updatePreloadProgress(100);
            section.reset();
            showBuildError();
            return;
          }
          updatePreloadProgress(100);

          if (section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                       SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                       viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                       SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
            if (cachedChapterTotalPageCount > 0) {
              if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
                float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
                section->currentPage = static_cast<int>(progress * section->pageCount);
              }
              cachedChapterTotalPageCount = 0;
            }
            if (section->currentPage >= section->pageCount && section->pageCount > 0) {
              section->currentPage = section->pageCount - 1;
            }
            updateBookmarkFlag();
            saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages());
            // This code runs on the render task, so notify that task directly.
            // A deferred update may otherwise wait for another input event.
            requestUpdate(true);
          }
          showPendingSyncSaveError();
          return;
        }
      }

      // Percentage and anchor navigation cannot know their final page before
      // the section table exists. Start from page zero and let the incremental
      // builder finish in slices; exact positioning is applied on completion.
      if (!loadedPartial && !pendingPageJump.has_value() && (pendingPercentJump || !pendingAnchor.empty()) &&
          section->beginIncrementalBuild(
              SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
              SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, preloadProgressFn)) {
        section->currentPage = 0;
        pageRenderRequested = true;
        beginPreloadProgress();
        lastIncrementalBuildTick = millis() - INCREMENTAL_BUILD_TICK_MS;
        return;
      }

      if (!loadedPartial) {
        beginPreloadProgress();
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn,
                                      preloadProgressFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          updatePreloadProgress(100);
          section.reset();
          showBuildError();
          return;
        }
        updatePreloadProgress(100);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
      updatePreloadProgress(100);
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
    applyCachedVisibleTextOffset();
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      if (section->isBuilding()) {
        section->abandonBuild();
      } else {
        section->clearCache();
      }
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->pageCount;
    }
  }
  pageRenderRequested = false;

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->isBuilding() || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  if (nextChapterPreload && nextChapterPreloadSpineIndex == nextSpineIndex) {
    return;
  }
  clearNextChapterPreload();

  Section cachedNextSection(epub, nextSpineIndex, renderer);
  if (cachedNextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    if (!cachedNextSection.isPartial()) return;
    LOG_DBG("ERS", "Next chapter has a partial section cache; extending it in preload");
  }

  nextChapterPreload = std::make_unique<Section>(epub, nextSpineIndex, renderer);
  const auto preloadProgressFn = [this](const uint8_t progress) { updatePreloadProgress(progress); };
  if (!nextChapterPreload->beginIncrementalBuild(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
          SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, preloadProgressFn)) {
    LOG_ERR("ERS", "Failed to start next-chapter preload: %d", nextSpineIndex);
    clearNextChapterPreload();
    return;
  }
  nextChapterPreloadSpineIndex = nextSpineIndex;
  LOG_DBG("ERS", "Started incremental next-chapter preload: %d", nextSpineIndex);
}

void EpubReaderActivity::advanceNextChapterPreload() {
  if (!nextChapterPreload || nextChapterPreloadSpineIndex != currentSpineIndex + 1) {
    clearNextChapterPreload();
    return;
  }

  const auto result = nextChapterPreload->buildNextChunk(1);
  if (result == Section::BuildResult::Failed) {
    LOG_ERR("ERS", "Next-chapter preload failed: %d", nextChapterPreloadSpineIndex);
    clearNextChapterPreload();
  } else if (result == Section::BuildResult::Complete) {
    LOG_DBG("ERS", "Next-chapter preload completed: %d", nextChapterPreloadSpineIndex);
    clearNextChapterPreload();
  }
}

void EpubReaderActivity::clearNextChapterPreload() {
  nextChapterPreload.reset();
  nextChapterPreloadSpineIndex = -1;
}

bool EpubReaderActivity::saveProgress(const int spineIndex, const int currentPage, const int pageCount,
                                      std::optional<uint32_t> visibleTextOffset) {
  if (!visibleTextOffset.has_value() && section && spineIndex == currentSpineIndex &&
      currentPage == section->currentPage && currentPage >= 0 && currentPage < section->pageCount) {
    visibleTextOffset = currentPageVisibleOffset.has_value()
                            ? currentPageVisibleOffset
                            : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  // A zero visible-text offset can belong to both the first page and a later
  // page that starts with non-text content. Persist the page-only format for
  // later pages so reopening cannot ambiguously resolve it to page zero.
  if (visibleTextOffset.has_value() && *visibleTextOffset == 0 && currentPage > 0) {
    visibleTextOffset.reset();
  }
  if (epub && pageCount > 0 && currentPage >= 0) {
    const float chapterProgress = std::clamp(static_cast<float>(currentPage + 1) / static_cast<float>(pageCount), 0.0f, 1.0f);
    const uint8_t bookProgress = static_cast<uint8_t>(
        std::clamp(epub->calculateProgress(spineIndex, chapterProgress) * 100.0f, 0.0f, 100.0f));
    std::string chapterTitle;
    const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
    if (tocIndex >= 0) chapterTitle = epub->getTocItem(tocIndex).title;
    READING_STATS.updateProgress(bookProgress, bookProgress >= 100, chapterTitle,
                                 static_cast<uint8_t>(chapterProgress * 100.0f));
  }
  const bool saved = EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, visibleTextOffset);
  if (saved && clearInitialProgressAfterSave_ && WeReadStore::clearInitialProgress(wereadBookId_)) {
    clearInitialProgressAfterSave_ = false;
  }
  return saved;
}

void EpubReaderActivity::applyCachedVisibleTextOffset() {
  if (!cachedVisibleTextOffset.has_value() || !section || currentSpineIndex != cachedSpineIndex) {
    return;
  }
  // Offset zero is valid for the first page, but it is ambiguous for later
  // pages (for example a page beginning with an image can also have no text
  // offset). In that case the persisted page number is the stronger resume
  // signal. Do not let the offset lookup move page 1+ back to page 0.
  if (*cachedVisibleTextOffset == 0 && section->currentPage > 0) {
    LOG_DBG("ERS", "Keeping cached page %d; visible offset 0 is ambiguous", section->currentPage);
    cachedVisibleTextOffset.reset();
    pendingOffsetJump.reset();
    return;
  }
  if (const auto page = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
    section->currentPage = *page;
    LOG_DBG("ERS", "Restored visible offset %u to page %u", static_cast<unsigned>(*cachedVisibleTextOffset),
            static_cast<unsigned>(*page));
    cachedVisibleTextOffset.reset();
    pendingOffsetJump.reset();
    pageRenderRequested = true;
  } else if (!section->isBuilding()) {
    LOG_DBG("ERS", "Visible offset %u is outside the rebuilt section; keeping page %d",
            static_cast<unsigned>(*cachedVisibleTextOffset), section->currentPage);
    cachedVisibleTextOffset.reset();
    pendingOffsetJump.reset();
  }
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (!section || section->currentPage < 0 || section->currentPage >= section->pageCount) return;
  cachedSpineIndex = currentSpineIndex;
  cachedChapterTotalPageCount = section->estimatedTotalPages();
  nextPageNumber = section->currentPage;
  if (currentPageVisibleOffset.has_value()) {
    cachedVisibleTextOffset = currentPageVisibleOffset;
  } else {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
  if (cachedVisibleTextOffset.has_value()) {
    LOG_DBG("ERS", "Remembered visible offset %u before reflow", static_cast<unsigned>(*cachedVisibleTextOffset));
  }
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
  pendingOffsetJump.reset();
  currentPageVisibleOffset.reset();
}

void EpubReaderActivity::beginPreloadProgress() {
  preloadProgressPercent = 0;
  lastDisplayedPreloadProgressPercent = 0;

  if (SETTINGS.statusBarProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::PRELOAD_PROGRESS) {
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::updatePreloadProgress(const uint8_t progress, const bool refreshStatusBar) {
  const uint8_t clampedProgress = std::min<uint8_t>(progress, 100);
  const uint8_t targetProgress =
      clampedProgress >= 100 ? 100 : static_cast<uint8_t>((clampedProgress / PRELOAD_PROGRESS_STEP) * PRELOAD_PROGRESS_STEP);
  if (!refreshStatusBar ||
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::PRELOAD_PROGRESS) {
    preloadProgressPercent = targetProgress;
    return;
  }

  if (lastDisplayedPreloadProgressPercent == 255) {
    lastDisplayedPreloadProgressPercent = 0;
  }
  if (targetProgress <= lastDisplayedPreloadProgressPercent) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom -
                           ((SETTINGS.statusBarProgressBarThickness + 1) * 2) + 1;
  const int progressBarHeight =
      ((SETTINGS.statusBarProgressBarThickness + 1) * 2) + orientedMarginBottom - 1;

  // Do not skip crossed thresholds when the parser reports a large jump.
  for (uint8_t step = static_cast<uint8_t>(lastDisplayedPreloadProgressPercent + PRELOAD_PROGRESS_STEP);
       step <= targetProgress; step = static_cast<uint8_t>(step + PRELOAD_PROGRESS_STEP)) {
    preloadProgressPercent = step;
    lastDisplayedPreloadProgressPercent = step;
    renderStatusBar();

    // displayWindow() uses physical panel coordinates. Reader portrait
    // coordinates are rotated, so use a fast full-buffer update in that mode.
    if (renderer.getScreenWidth() == HalDisplay::DISPLAY_WIDTH &&
        renderer.getScreenHeight() == HalDisplay::DISPLAY_HEIGHT) {
      renderer.displayWindow(0, progressBarY, renderer.getScreenWidth(), progressBarHeight);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  // The same page may be rendered for BW, image cleanup, and multiple grayscale
  // bands. ImageBlock owns a bounded payload cache for those passes; release it
  // automatically on every exit path so it never survives a page turn.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  // The status bar contains the current chapter title and can contain CJK
  // glyphs too. Render it while scan mode is active so it joins the same
  // bounded fallback-font prewarm as the page body.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // EPUB text AA currently re-renders the full page for every grayscale strip
  // and can turn an ordinary page flip into a minute-plus render on X4. Keep
  // image grayscale, but do not route text-only pages through grayscale.
  const bool needsTextGrayscale = false;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Image pages bypass the normal refresh cadence. Preserve the explicit
      // manual clean pass before their double-FAST image pipeline.
      if (manualRefreshPending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    std::unique_ptr<uint8_t[]> scratch;
    int stripRows = 0;
    const int stripCandidates[] = {gh, gh / 2, gh / 4, 160, 80};
    for (const int candidate : stripCandidates) {
      if (candidate <= 0 || candidate > gh || candidate == stripRows) {
        continue;
      }
      scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * candidate);
      if (scratch) {
        stripRows = candidate;
        break;
      }
    }
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch; skipping AA this page");
    } else {
      LOG_DBG("ERS", "Using grayscale strip height: %d rows", stripRows);
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    preloadProgressPercent);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  const CrossPointPosition currentPosition = getCurrentPosition();
  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, currentPosition);
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    entry.hasVisibleTextOffset = currentPosition.hasVisibleTextOffset;
    entry.visibleTextOffset = currentPosition.visibleTextOffset;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const bool ok = BookmarkFile::save(epub->getPath(), cachedBookmarks);
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks for: %s", epub->getPath().c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

bool EpubReaderActivity::isAtEndOfBook() const {
  return epub && currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
}

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (!epub || !isAtEndOfBook()) return;
  currentSpineIndex = std::max(0, epub->getSpineItemsCount() - 1);
  nextPageNumber = 0;
  pendingPageJump = std::numeric_limits<uint16_t>::max();
  clearNextChapterPreload();
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const auto offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                            ? currentPageVisibleOffset
                            : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
    if (offset.has_value()) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  return localPos;
}
