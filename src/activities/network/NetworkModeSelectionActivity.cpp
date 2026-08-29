#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CREATE_HOTSPOT,
    StrId::STR_NUTSTORE_SYNC, StrId::STR_AIRPAGE_TITLE};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC, StrId::STR_HOTSPOT_DESC,
    StrId::STR_NUTSTORE_DESC, StrId::STR_AIRPAGE_DESC};
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < MENU_ITEM_COUNT; ++i) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems_[i] = item;
  }
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = rowItems_;
  props.count = MENU_ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props, true);
  screen.list(props);
}

void NetworkModeSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= MENU_ITEM_COUNT) return;
  app.clearTapFlash();
  nav.selected = index;
  onModeSelected(static_cast<NetworkMode>(index));
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
