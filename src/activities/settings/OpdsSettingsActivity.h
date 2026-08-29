#pragma once

#include <string>

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"

/**
 * Edit screen for a single OPDS server.
 * Shows Name, URL, Username, Password fields and a Delete option.
 * Used for both adding new servers and editing existing ones.
 */
class OpdsSettingsActivity final : public UiListActivity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1)
      : UiListActivity("OpdsSettings", renderer, mappedInput), serverIndex(serverIndex) {}

  void onEnter() override;

 private:
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;

  static constexpr int BASE_ITEMS = 4;
  std::string rowValues[BASE_ITEMS + 1];
  freeink::ui::ListItem rowItems[BASE_ITEMS + 1]{};

  int getMenuItemCount() const;
  void handleSelection();
  bool saveServer();
  int listCount() const override { return getMenuItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawChrome() override;
  void drawFooter() override;
};
