#pragma once

#include "activities/UiListActivity.h"

class ExtensionsMenuActivity final : public UiListActivity {
  static constexpr int MENU_ITEMS = 8;

  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

 public:
  explicit ExtensionsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Extensions", renderer, mappedInput) {}

 private:
  int listCount() const override { return MENU_ITEMS; }
};
