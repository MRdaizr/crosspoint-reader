#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginEvents.h"

bool ReaderActivity::handleProgressRecoveryError() {
  if (!progressRecoveryFailed_) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onGoHome();
  return true;
}

void ReaderActivity::markPageRendered() {
  resumeGate.markPageRendered();
  renderedForEvents.store(true, std::memory_order_release);
  const int8_t turn = pendingSessionTurn.exchange(0, std::memory_order_acq_rel);
  readerSession.noteTurn(turn > 0, turn != 0);
  const auto epoch = halClock.hasValidTime() ? halClock.nowUtc() : 0;
  readerSession.onRenderComplete(millis(), epoch, getScreenshotInfo().progressPercent * 100);
}

void ReaderActivity::flushReaderSession() {
  if (!pluginevents::anySubscriber(pluginevents::Event::ReaderSession) || !readerSession.takeForFlush()) return;
  // A document hash is computed only for a subscribed, non-empty session.
  const std::string document = KOReaderDocumentId::calculate(bookPath);
  if (document.size() != 32) return;
  char start[24], end[24], duration[16], first[8], last[8];
  snprintf(start, sizeof(start), "%lld", static_cast<long long>(readerSession.startTime()));
  snprintf(end, sizeof(end), "%lld", static_cast<long long>(readerSession.endTime()));
  snprintf(duration, sizeof(duration), "%u", readerSession.durationSeconds());
  snprintf(first, sizeof(first), "%u", readerSession.startProgressBp());
  snprintf(last, sizeof(last), "%u", readerSession.endProgressBp());
  const pluginevents::Var vars[] = {{"book", bookPath.c_str()},     {"document", document.c_str()},
                                    {"start_time", start},          {"end_time", end},
                                    {"duration_seconds", duration}, {"start_progress_bp", first},
                                    {"end_progress_bp", last},      {"progress_scale", "10000"}};
  pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
}

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Explicit compatibility factory; default routing prepares reflow.
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
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, filePath,
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Do not repeatedly reopen a book that cannot load or build its first page.
  // A successful format render publishes the replacement below.
  if (!APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

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

  const std::string title = getBookTitle();
  READING_STATS.beginSession(bookPath, title, getBookAuthor(), getBookThumbBmpPath(), getInitialProgressPercent());
  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered() {
  if (!resumeGate.takeRememberRequest()) return;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  const pluginevents::Var vars[] = {{"book", bookPath.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, vars, 1);
}

void ReaderActivity::onExit() {
  Activity::onExit();
  // A quick sleep/back can arrive before loop() consumes the first render.
  rememberBookOnceRendered();
  flushReaderSession();
  if (renderedForEvents.load(std::memory_order_acquire) &&
      pluginevents::anySubscriber(pluginevents::Event::ReaderExit)) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", getScreenshotInfo().progressPercent);
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()}, {"percent", percent}};
    pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  }
  // Derived hooks must release parser/cache resources while the Activity
  // context is still valid (for example EPUB image extraction and stats).
  onBookExited();
  // Metrics, mini bitmaps and ligatures are rebuildable. Do not leave them
  // pinning the heap after the derived reader has released its section.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
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
  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions && endOfBookOptions->menuActive()) {
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
  markPageRendered();
}

void ReaderActivity::render(RenderLock&&) {
  if (progressRecoveryFailed_) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_READER_PROGRESS_READ_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
  if (isAtEndOfBook()) {
    renderEndOfBook(mappedInput);
    return;
  }
  renderBook();
}
