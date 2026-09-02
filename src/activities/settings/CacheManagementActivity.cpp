#include "CacheManagementActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "ClearCacheActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId MENU_NAMES[CacheManagementActivity::MENU_ITEMS] = {
    StrId::STR_CLEAR_READING_CACHE,
    StrId::STR_CLEAR_STATS,
    StrId::STR_CLEAR_CARDS,
    StrId::STR_CLEAR_POMODORO,
    StrId::STR_CLEAR_ALL,
};
}

void CacheManagementActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < MENU_ITEMS; ++i) {
    rowItems[i].label = I18N.get(MENU_NAMES[i]);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    rowItems[i].icon = {};
  }
}

void CacheManagementActivity::activateIndex(const int index) {
  if (index < 0 || index >= MENU_ITEMS) return;
  app.clearTapFlash();
  nav.selected = index;

  static constexpr ClearCacheType CACHE_TYPES[MENU_ITEMS] = {
      ClearCacheType::Reading,
      ClearCacheType::ReadingStats,
      ClearCacheType::Flashcards,
      ClearCacheType::PomodoroStats,
      ClearCacheType::All,
  };
  startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput, CACHE_TYPES[index]),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void CacheManagementActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = rowItems;
  props.count = MENU_ITEMS;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}
