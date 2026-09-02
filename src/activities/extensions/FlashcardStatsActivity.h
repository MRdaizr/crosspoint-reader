#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FlashcardScheduler.h"
#include "FlashcardStatsStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class FlashcardStatsActivity final : public Activity, private UiAppHost {
 public:
  enum class View : uint8_t { Overview, Decks, DeckDetail, Calendar, DayDetail };

 private:
  struct DeckSummary {
    std::string name;
    uint32_t cardCount = 0;
    uint32_t learnedCount = 0;
    uint32_t dueCount = 0;
    uint32_t learningCount = 0;
    uint32_t relearningCount = 0;
    uint32_t reviewCount = 0;
    uint32_t lapses = 0;
    uint32_t totalReviews = 0;
    uint16_t currentStreak = 0;
    uint16_t maxStreak = 0;
    uint16_t completedToday = 0;
  };

  struct Totals {
    uint32_t cardCount = 0;
    uint32_t learnedCount = 0;
    uint32_t dueCount = 0;
    uint32_t learningCount = 0;
    uint32_t relearningCount = 0;
    uint32_t reviewCount = 0;
    uint32_t lapses = 0;
    uint32_t totalReviews = 0;
    uint32_t completedToday = 0;
    uint16_t fallbackCurrentStreak = 0;
    uint16_t fallbackMaxStreak = 0;
  };

  ButtonNavigator buttonNavigator;
  View view = View::Overview;
  int selectedIndex = 0;
  uint32_t selectedDayOrdinal = 0;
  std::vector<DeckSummary> decks;
  std::vector<FlashcardDailyStats> dailyEntries;
  Totals totals;

  std::vector<std::string> fuiLabels_;
  std::vector<std::string> fuiValues_;
  std::vector<freeink::ui::ListItem> fuiRows_;
  freeink::ui::ListNav fuiNav_;

  void refreshData();
  void moveVertical(int delta);
  void renderOverview();
  void renderDeckDetail();
  void renderCalendar();
  void renderDayDetail();
  void drawFooter(const char* confirmLabel = nullptr) const;
  uint32_t referenceDayOrdinal() const;
  const FlashcardDailyStats* statsForDay(uint32_t ordinal) const;
  FlashcardDailyStats recentStats(uint32_t days) const;
  uint32_t completedForDay(uint32_t ordinal) const;
  uint32_t recentCompleted(uint32_t days) const;
  uint32_t currentStreak() const;
  uint32_t maxStreak() const;

  static void fuiScreen(UiScreen& screen, void* user);
  static void onFuiRow(const freeink::ui::ActionEvent& event, void* user);
  void buildFuiScreen(UiScreen& screen);
  bool routeFuiTouch();

 public:
  explicit FlashcardStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FlashcardStats", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;
};
