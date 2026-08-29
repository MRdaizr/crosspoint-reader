#pragma once

#include <vector>

#include "TodoStore.h"
#include "activities/UiListActivity.h"

class TodoActivity final : public UiListActivity {
 public:
  explicit TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Todos", renderer, mappedInput) {}

  void onEnter() override;
  bool allowPowerSaving() override { return true; }

 private:
  std::vector<TodoItem> items;
  std::vector<std::string> rowTitles;
  std::vector<std::string> rowDates;
  std::vector<freeink::ui::ListItem> rowItems;
  bool loadFailed = false;

  bool reload(uint32_t selectedId = 0);
  void rebuildRowItems();
  int listCount() const override { return static_cast<int>(items.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
};
