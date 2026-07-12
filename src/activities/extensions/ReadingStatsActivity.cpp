#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {

std::string formatDuration(uint32_t seconds) {
  const uint32_t minutes = (seconds + 30U) / 60U;
  if (minutes < 60U) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(minutes));
    return buf;
  }
  const uint32_t hours = minutes / 60U;
  const uint32_t remainingMinutes = minutes % 60U;
  char buf[24];
  snprintf(buf, sizeof(buf), "%luh%02lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainingMinutes));
  return buf;
}

uint32_t totalReadingSeconds(const std::vector<ReadingStatEntry>& entries) {
  uint32_t total = 0;
  for (const auto& entry : entries) total += entry.totalSeconds;
  return total;
}

}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  // Load before the first render so the initial screen never uses an empty lazy-load result.
  READING_STATS.loadFromFile();
  displayedEntries = READING_STATS.getEntries();
  selectedIndex = 0;
  requestUpdate(true);
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int itemCount = static_cast<int>(displayedEntries.size());
  if (itemCount <= 0) return;
  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  const auto& entries = displayedEntries;
  char line[64];
  snprintf(line, sizeof(line), "%s %s", tr(STR_TOTAL_READING_TIME), formatDuration(totalReadingSeconds(entries)).c_str());
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, top + 10, line, true, EpdFontFamily::BOLD);

  const int listTop = top + 45;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop + 10, tr(STR_NO_READING_STATS));
  } else {
    std::string visibleText;
    for (const auto& entry : entries) {
      visibleText += entry.title;
      visibleText += '\n';
    }
    const int titleFontId = DynamicFont::fontForCjkText(renderer, visibleText.c_str(), 0);
    DynamicFont::prewarmIfSdFont(renderer, titleFontId, visibleText);
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(entries.size()), selectedIndex,
                 [&entries](int index) { return entries[index].title; }, nullptr, nullptr,
                 [&entries](int index) { return formatDuration(entries[index].totalSeconds); }, false, nullptr,
                 titleFontId);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
