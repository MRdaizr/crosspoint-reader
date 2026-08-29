#pragma once

#include <I18n.h>

#include <string>

#include "activities/UiListActivity.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, and authenticate options.
 */
class KOReaderSettingsActivity final : public UiListActivity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("KOReaderSettings", renderer, mappedInput) {}

  void onEnter() override;

 private:
  static constexpr int MENU_ITEMS = 5;
  std::string rowValues[MENU_ITEMS];
  freeink::ui::ListItem rowItems[MENU_ITEMS]{};

  void handleSelection();
  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_KOREADER_SYNC); }
};
