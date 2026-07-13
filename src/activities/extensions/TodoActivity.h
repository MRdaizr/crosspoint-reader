#pragma once

#include <vector>

#include "TodoStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TodoActivity final : public Activity {
 public:
  explicit TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Todos", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::vector<TodoItem> items;
  int selectedIndex = 0;
  bool loadFailed = false;

  bool reload(uint32_t selectedId = 0);
};
