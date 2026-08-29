#pragma once

#include <I18n.h>

#include <string>

#include "activities/UiListActivity.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public UiListActivity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("StatusBarSettings", renderer, mappedInput) {}

  static constexpr int MAX_STATUS_BAR_ITEMS = 11;

  void onEnter() override;

 private:
  // Decided in onEnter() based on halClock.isAvailable() so clock entries are hidden on X4.
  int visibleItemCount = 0;

  int listCount() const override { return visibleItemCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_CUSTOMISE_STATUS_BAR); }
  void drawFooter() override;

  void handleSelection();
  std::string rowValueText(int index);

  std::string rowValues_[MAX_STATUS_BAR_ITEMS];
  freeink::ui::ListItem rowItems_[MAX_STATUS_BAR_ITEMS]{};
};
