#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class TxtReaderMenuActivity final : public UiListActivity {
 public:
  enum class MenuAction { GO_TO_PERCENT, ROTATE_SCREEN, SCREENSHOT, GO_HOME };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                 int currentPage, int totalPages, int progressPercent, uint8_t currentOrientation);

  void onEnter() override;
  void onExit() override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems();

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;
  const char* headerTitle() const override { return title.c_str(); }

  const std::vector<MenuItem> menuItems;
  std::vector<freeink::ui::ListItem> rowItems;
  std::string title;
  int currentPage = 0;
  int totalPages = 0;
  int progressPercent = 0;
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
};
