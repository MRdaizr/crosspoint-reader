#pragma once

#include <I18n.h>

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows credentials, matching/sync behavior, and authentication options.
 */
class KOReaderSettingsActivity final : public UiListActivity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("KOReaderSettings", renderer, mappedInput) {}

  void onEnter() override;
  void render(RenderLock&&) override;
  static constexpr int MENU_ITEMS = 9;

 private:
  std::string rowValues[MENU_ITEMS];
  freeink::ui::ListItem rowItems[MENU_ITEMS]{};
  OptionPopup profilePopup;

  void handleSelection();
  bool handleCustomInput() override;
  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_KOREADER_SYNC); }
};
