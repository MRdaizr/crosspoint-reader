#pragma once

#include <cstdint>
#include <string>

#include "AchievementsStore.h"
#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
 public:
  enum class View : uint8_t { Overview, Books, Calendar, DayDetail, BookDetail, Profile, Achievements };

 private:
  ButtonNavigator buttonNavigator;
  View view = View::Overview;
  int selectedIndex = 0;
  uint32_t selectedDayOrdinal = 0;
  std::string selectedBookPath;

  void moveVertical(int delta);
  void renderOverview();
  void renderBooks();
  void renderCalendar();
  void renderDayDetail();
  void renderBookDetail();
  void renderProfile();
  void renderAchievements();
  void drawFooter(const char* confirmLabel = "Open") const;

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;
};
