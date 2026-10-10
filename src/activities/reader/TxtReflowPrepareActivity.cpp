#include "TxtReflowPrepareActivity.h"

#include <CrossPointSettings.h>
#include <I18n.h>
#include <Memory.h>

#include "EpubReaderActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TxtProgressBridge.h"

TxtReflowPrepareActivity::TxtReflowPrepareActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path,
                                                   bool fastRefresh)
    : Activity("TxtReflowPrepare", renderer, input), path(std::move(path)), fastRefresh(fastRefresh) {}
void TxtReflowPrepareActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  requestUpdate();
}
void TxtReflowPrepareActivity::onExit() {
  // ActivityManager serializes exit with its render task. No private task, no
  // recursive render mutex, and no stale callback can outlive this activity.
  cancelRequested.store(true, std::memory_order_release);
  readyEpub.reset();
  Activity::onExit();
}
bool TxtReflowPrepareActivity::cancelled(void* context) {
  return static_cast<TxtReflowPrepareActivity*>(context)->cancelRequested.load(std::memory_order_acquire);
}
void TxtReflowPrepareActivity::progress(void* context, uint32_t bytes, uint32_t total) {
  auto& self = *static_cast<TxtReflowPrepareActivity*>(context);
  if (total) {
    const uint8_t value = uint64_t(bytes) * 100 / total;
    if (value > self.percent.load()) self.percent.store(value);
  }
  const uint32_t now = millis();
  const uint8_t value = self.percent.load();
  if (now - self.lastDraw >= 2000 && (self.lastPercent == 255 || value >= self.lastPercent + 5)) {
    self.lastDraw = now;
    self.lastPercent = value;
    self.drawStatus();  // Already on the render task with its lock.
  }
  vTaskDelay(1);  // Keep GPIO cancellation polling and the watchdog alive.
}
bool TxtReflowPrepareActivity::handleHomeGesture() {
  homeRequested = true;
  cancelRequested.store(true, std::memory_order_release);
  return true;
}
void TxtReflowPrepareActivity::loop() {
  const auto current = state.load(std::memory_order_acquire);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested.store(true, std::memory_order_release);
  if (current == State::Pending || current == State::Working) return;
  if (cancelRequested.load()) {
    if (homeRequested)
      onGoHome();
    else
      activityManager.goToFileBrowser(path);
    return;
  }
  if (current == State::Ready) {
    RenderLock lock(false);
    if (!lock.ownsLock()) return;
    auto next = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(readyEpub), fastRefresh);
    if (!next) {
      LOG_ERR("TXT", "OOM entering unified reader");
      state.store(State::Failed);
      lock.unlock();
      requestUpdate();
      return;
    }
    lock.unlock();
    activityManager.replaceActivity(std::move(next));
  } else if ((current == State::Failed || current == State::Compatibility) &&
             mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // create() deliberately remains the compatibility factory; ActivityManager
    // owns the default TXT/MD -> preparation route, so this cannot recurse.
    auto next = ReaderActivity::create(renderer, mappedInput, path, fastRefresh);
    if (next)
      activityManager.replaceActivity(std::move(next));
    else {
      LOG_ERR("TXT", "Cannot allocate compatibility reader");
      requestUpdate();
    }
  }
}
void TxtReflowPrepareActivity::render(RenderLock&&) {
  if (state.load() == State::Pending) {
    state.store(State::Working, std::memory_order_release);
    drawStatus();
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    const Epub::LoadCallbacks callbacks{this, cancelled, progress};
    State result = State::Failed;
    if (epub && epub->load(true, true, &callbacks) && epub->getTextSourceMap()) {
      TxtProgressBridge bridge(path, epub->getCachePath(), *epub->getTextSourceMap());
      const auto migration = bridge.migrateToUnified(&callbacks);
      if (migration == TxtProgressBridge::Result::NeedsCompatibility)
        result = State::Compatibility;
      else if (migration == TxtProgressBridge::Result::Ready || migration == TxtProgressBridge::Result::Migrated ||
               migration == TxtProgressBridge::Result::NoProgress) {
        readyEpub = std::move(epub);
        result = State::Ready;
      }
    } else if (!epub) {
      LOG_ERR("TXT", "OOM preparing unified text");
    }
    if (cancelRequested.load()) {
      readyEpub.reset();
      result = State::Cancelled;
    }
    state.store(result, std::memory_order_release);
  }
  drawStatus();
}
void TxtReflowPrepareActivity::drawStatus() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_TEXT_PREPARING));
  const auto current = state.load();
  const bool failed = current == State::Failed || current == State::Compatibility;
  if (failed) {
    renderer.drawCenteredText(
        UI_10_FONT_ID, height / 2 - metrics.verticalSpacing,
        current == State::Compatibility ? tr(STR_TEXT_PROGRESS_UNAVAILABLE) : tr(STR_TEXT_PREPARE_FAILED));
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 + renderer.getLineHeight(UI_10_FONT_ID),
                              tr(STR_TEXT_COMPATIBILITY_READER));
  } else {
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, height / 2, width - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        percent.load(), 100);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), failed ? tr(STR_CONFIRM) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
