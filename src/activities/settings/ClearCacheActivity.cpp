#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "AchievementsStore.h"
#include "PomodoroStatsStore.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
StrId titleFor(ClearCacheType type) {
  switch (type) {
    case ClearCacheType::Flashcards:
      return StrId::STR_CLEAR_FLASHCARD_CACHE;
    case ClearCacheType::ReadingStats:
      return StrId::STR_CLEAR_READING_STATS_CACHE;
    case ClearCacheType::PomodoroStats:
      return StrId::STR_CLEAR_POMODORO_CACHE;
    case ClearCacheType::Reading:
    default:
      return StrId::STR_CLEAR_READING_CACHE;
  }
}

StrId warningFor(ClearCacheType type, int line) {
  if (type == ClearCacheType::Flashcards) {
    return line == 1 ? StrId::STR_CLEAR_FLASHCARD_WARNING_1
                     : line == 2 ? StrId::STR_CLEAR_FLASHCARD_WARNING_2 : StrId::STR_CLEAR_FLASHCARD_WARNING_3;
  }
  if (type == ClearCacheType::ReadingStats) {
    return line == 1 ? StrId::STR_CLEAR_READING_STATS_WARNING_1
                     : line == 2 ? StrId::STR_CLEAR_READING_STATS_WARNING_2 : StrId::STR_NONE_OPT;
  }
  if (type == ClearCacheType::PomodoroStats) {
    return line == 1 ? StrId::STR_CLEAR_POMODORO_WARNING_1
                     : line == 2 ? StrId::STR_CLEAR_POMODORO_WARNING_2 : StrId::STR_NONE_OPT;
  }
  return line == 1 ? StrId::STR_CLEAR_CACHE_WARNING_1
                   : line == 2 ? StrId::STR_CLEAR_CACHE_WARNING_2 : StrId::STR_CLEAR_CACHE_WARNING_3;
}
}  // namespace

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(titleFor(cacheType)));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 35, I18N.get(warningFor(cacheType, 1)), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 5, I18N.get(warningFor(cacheType, 2)), true,
                              EpdFontFamily::BOLD);
    const StrId thirdWarning = warningFor(cacheType, 3);
    if (thirdWarning != StrId::STR_NONE_OPT) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, I18N.get(thirdWarning), true);
    }
    if (cacheType == ClearCacheType::Reading) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 45, tr(STR_CLEAR_CACHE_WARNING_4), true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  if (cacheType == ClearCacheType::ReadingStats) {
    const bool existed = Storage.exists("/.crosspoint/reading_stats.json");
    READING_STATS.clear();
    const bool achievementFileExisted = Storage.exists("/.crosspoint/achievements.json");
    ACHIEVEMENTS.clear();
    clearedCount = (existed ? 1 : 0) + (achievementFileExisted ? 1 : 0);
    failedCount = (existed && Storage.exists("/.crosspoint/reading_stats.json") ? 1 : 0) +
                  (achievementFileExisted && Storage.exists("/.crosspoint/achievements.json") ? 1 : 0);
    state = failedCount == 0 ? SUCCESS : FAILED;
    requestUpdate();
    return;
  }

  if (cacheType == ClearCacheType::PomodoroStats) {
    const bool existed = Storage.exists("/.crosspoint/pomodoro_stats.json");
    POMODORO_STATS.clear();
    clearedCount = existed ? 1 : 0;
    failedCount = existed && Storage.exists("/.crosspoint/pomodoro_stats.json") ? 1 : 0;
    state = failedCount == 0 ? SUCCESS : FAILED;
    requestUpdate();
    return;
  }

  if (cacheType == ClearCacheType::Flashcards) {
    clearedCount = 0;
    failedCount = 0;
    if (Storage.exists("/.crosspoint/flashcards")) {
      if (Storage.removeDir("/.crosspoint/flashcards")) ++clearedCount;
      else ++failedCount;
    }
    auto decks = Storage.open("/flashcards");
    if (decks && decks.isDirectory()) {
      char name[160];
      for (auto file = decks.openNextFile(); file; file = decks.openNextFile()) {
        file.getName(name, sizeof(name));
        const std::string filename(name);
        file.close();
        if (filename.size() > 4 && filename.rfind(".idx") == filename.size() - 4) {
          const std::string path = std::string("/flashcards/") + filename;
          if (Storage.remove(path.c_str())) ++clearedCount;
          else ++failedCount;
        }
      }
      decks.close();
    }
    state = failedCount == 0 ? SUCCESS : (clearedCount > 0 ? SUCCESS : FAILED);
    requestUpdate();
    return;
  }

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      requestUpdateAndWait();

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
