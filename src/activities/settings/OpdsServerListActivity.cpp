#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // In settings mode, append a virtual "Add Server" item; in picker mode, only show real servers
  if (!pickerMode) {
    count++;
  }
  return count;
}

void OpdsServerListActivity::onEnter() {
  UiListActivity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  nav.selected = 0;
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
  if (nav.selected < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(nav.selected));
      if (server) {
        activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server));
      }
    }
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    nav.selected = 0;
    requestUpdate();
  };

  if (nav.selected < serverCount) {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, nav.selected), resultHandler);
  } else {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void OpdsServerListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int itemCount = getItemCount();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (itemCount == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }
  const auto& servers = OPDS_STORE.getServers();
  const int serverCount = static_cast<int>(servers.size());
  rowLabels.clear();
  rowSubtitles.clear();
  rowItems.clear();
  rowLabels.reserve(static_cast<size_t>(itemCount));
  rowSubtitles.reserve(static_cast<size_t>(itemCount));
  rowItems.reserve(static_cast<size_t>(itemCount));
  for (int i = 0; i < itemCount; ++i) {
    if (i < serverCount) {
      const auto& server = servers[static_cast<size_t>(i)];
      rowLabels.push_back(server.name.empty() ? server.url : server.name);
      rowSubtitles.push_back(server.name.empty() ? std::string() : server.url);
    } else {
      rowLabels.emplace_back(tr(STR_ADD_SERVER));
      rowSubtitles.emplace_back();
    }
    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    item.subtitle = rowSubtitles.back().empty() ? nullptr : rowSubtitles.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems.push_back(item);
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  syncListViewport(screen, props, true);
  screen.list(props);
}

void OpdsServerListActivity::activateIndex(const int index) {
  if (index < 0 || index >= getItemCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  handleSelection();
}

void OpdsServerListActivity::onBackButton() {
  if (pickerMode) {
    activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
  } else {
    finish();
  }
}
