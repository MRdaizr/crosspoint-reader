#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * When pickerMode is true, selecting a server navigates to the OPDS browser
 * instead of opening the editor (used from the home screen).
 */
class OpdsServerListActivity final : public UiListActivity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false)
      : UiListActivity("OpdsServerList", renderer, mappedInput), pickerMode(pickerMode) {}

  void onEnter() override;

 private:
  bool pickerMode = false;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowSubtitles;
  std::vector<freeink::ui::ListItem> rowItems;

  int getItemCount() const;
  void handleSelection();
  int listCount() const override { return getItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return tr(STR_OPDS_SERVERS); }
};
