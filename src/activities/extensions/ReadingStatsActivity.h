#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AchievementsStore.h"
#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity, private UiAppHost {
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
  void renderCalendar();
  void renderDayDetail();
  void renderBookDetail();
  void renderProfile();
  void renderAchievements();
  void drawFooter(const char* confirmLabel = "Open") const;

  static void fuiScreen(UiScreen& screen, void* user);
  static void onFuiRow(const freeink::ui::ActionEvent& event, void* user);
  void buildFuiScreen(UiScreen& screen);
  bool routeFuiTouch();

  // Row strings live for the entire render generation.  FreeInkUI keeps
  // pointers to these strings in its interaction table, so do not build them
  // as temporaries inside the ListProps loop.
  std::vector<std::string> fuiLabels_;
  std::vector<std::string> fuiValues_;
  std::vector<freeink::ui::ListItem> fuiRows_;
  freeink::ui::ListNav fuiNav_;

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;
};
