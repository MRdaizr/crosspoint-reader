#include "TxtReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const int currentPage, const int totalPages,
                                             const int progressPercent, const uint8_t currentOrientation)
    : UiListActivity("TxtReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems()),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      progressPercent(progressPercent),
      pendingOrientation(currentOrientation) {
  rowItems.reserve(menuItems.size());
  for (size_t i = 0; i < menuItems.size(); ++i) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i].labelId);
    item.icon = {};
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

std::vector<TxtReaderMenuActivity::MenuItem> TxtReaderMenuActivity::buildMenuItems() {
  return {{MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT},
          {MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION},
          {MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON},
          {MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON}};
}

void TxtReaderMenuActivity::onEnter() {
  UiListActivity::onEnter();
}

void TxtReaderMenuActivity::onExit() { UiListActivity::onExit(); }

void TxtReaderMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const auto selectedAction = menuItems[static_cast<size_t>(index)].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    pendingOrientation = static_cast<uint8_t>((pendingOrientation + 1) % orientationLabels.size());
    requestUpdate();
    return;
  }

  setResult(TxtMenuResult{static_cast<int>(selectedAction), pendingOrientation});
  finish();
}

bool TxtReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (nav.selected >= 0 && nav.selected < listCount()) activateIndex(nav.selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = TxtMenuResult{-1, pendingOrientation};
    setResult(std::move(result));
    finish();
    return true;
  }
  return false;
}

void TxtReaderMenuActivity::drawChrome() {
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight}, title.c_str());
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(progressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());
}

void TxtReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (pendingOrientation < orientationLabels.size()) {
    rowItems[1].value = I18N.get(orientationLabels[pendingOrientation]);
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}
