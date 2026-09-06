#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/reader/ReaderUtils.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    LOG_ERR("READER", "Cannot create reader for an empty path");
    return nullptr;
  }

  // The factory only selects the format-specific Activity.  Each concrete
  // reader loads its book in onEnter(), so a failed allocation or malformed
  // file follows the same deferred ActivityManager lifecycle as every other
  // screen and never performs ZIP/SD work on the caller's stack.
  if (isXtcFile(path)) {
    return makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }
  if (isTxtFile(path)) {
    return makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }
  return makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

bool ReaderActivity::handleBackNavigation(const char* filePath) {
  return ReaderUtils::handleBackNavigation(
      mappedInput, activityManager, filePath,
      {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::applyInitialOrientation() {
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (bookPath.empty() || !Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "Cannot enter reader for missing path: %s", bookPath.c_str());
    finish();
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    LOG_ERR("READER", "Failed to load book: %s", bookPath.c_str());
    finish();
    return;
  }

  // Let the format hook restore its position/cache first. In particular, XTC
  // needs its persisted page before the shared session metadata captures the
  // initial progress percentage.
  onBookEntered();

  // Metadata and persisted state are shared by all three readers.
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  const std::string title = getBookTitle();
  RECENT_BOOKS.addBook(bookPath, title, getBookAuthor(), getBookThumbBmpPath());
  READING_STATS.beginSession(bookPath, title, getBookAuthor(), getBookThumbBmpPath(), getInitialProgressPercent());
  requestUpdate();
}

void ReaderActivity::onExit() {
  Activity::onExit();
  // Derived hooks must release parser/cache resources while the Activity
  // context is still valid (for example EPUB image extraction and stats).
  onBookExited();
  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
}

bool ReaderActivity::handleForcedRefresh() {
  // Only touch the shared refresh state while holding the render lock. The
  // request originates from the input task, while the next page is rendered
  // on ActivityManager's render task.
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}

ReaderRenderSpec ReaderActivity::currentReaderRenderSpec() const {
  return SETTINGS.readerRenderSpec(static_cast<uint16_t>(renderer.getScreenWidth()),
                                   static_cast<uint16_t>(renderer.getScreenHeight()));
}

ReaderRenderSpec ReaderActivity::currentReaderRenderSpec(const uint16_t viewportWidth,
                                                         const uint16_t viewportHeight) const {
  return SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded(const bool atEndOfBook) {
  if (atEndOfBook || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::handleEndOfBookMenu(const bool atEndOfBook, const bool suppressConfirmRelease) {
  if (!atEndOfBook || suppressConfirmRelease || !endOfBookOptionsReady.load(std::memory_order_acquire) ||
      !endOfBookOptions || !endOfBookOptions->menuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(std::move(openPath));
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      // The format reader owns the actual page sentinel and restores its last
      // page through the normal render path.
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }
  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool atEndOfBook, const bool prevTriggered,
                                             const bool nextTriggered) {
  if (!atEndOfBook) return false;
  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions &&
      endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::renderEndOfBook(const MappedInputManager& input) {
  if (!endOfBookOptions) {
    endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
    if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
  }

  renderer.clearScreen();
  if (endOfBookOptions) {
    endOfBookOptions->loadOnce(bookPath);
    endOfBookOptionsReady.store(true, std::memory_order_release);
    endOfBookOptions->render(renderer, input);
  }
  onEndOfBookRendered();
  renderer.displayBuffer();
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    renderEndOfBook(mappedInput);
    return;
  }
  renderBook();
}
