#pragma once

#include <vector>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<ReadingStatEntry> displayedEntries;

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
