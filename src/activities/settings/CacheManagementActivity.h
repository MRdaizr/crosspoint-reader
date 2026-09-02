#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

class CacheManagementActivity final : public UiListActivity {
 public:
  static constexpr int MENU_ITEMS = 5;

  explicit CacheManagementActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("CacheManagement", renderer, mappedInput) {}

  void onEnter() override;

 private:
  freeink::ui::ListItem rowItems[MENU_ITEMS]{};

  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_CACHE_DATA_MANAGEMENT); }
};
