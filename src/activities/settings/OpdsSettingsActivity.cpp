#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password.
// Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 4;
}  // namespace

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  isNewServer = (serverIndex < 0);
  showSaveError = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }

  static constexpr StrId fieldNames[BASE_ITEMS] = {StrId::STR_SERVER_NAME, StrId::STR_OPDS_SERVER_URL,
                                                    StrId::STR_USERNAME, StrId::STR_PASSWORD};
  for (int i = 0; i < BASE_ITEMS; ++i) {
    rowItems[i].label = I18N.get(fieldNames[i]);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    rowItems[i].icon = {};
  }
  rowItems[BASE_ITEMS].label = tr(STR_DELETE_SERVER);
  rowItems[BASE_ITEMS].actionValue = BASE_ITEMS;
  rowItems[BASE_ITEMS].icon = {};
}

bool OpdsSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    success = OPDS_STORE.addServer(editServer);
    if (success) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::handleSelection() {
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (nav.selected == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME),
                                                                   editServer.name, 63, InputType::Text),
                           handler);
  } else if (nav.selected == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_SERVER_URL),
                                                                   prefillUrl, 127, InputType::Url),
                           handler);
  } else if (nav.selected == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                   editServer.username, 63, InputType::Text),
                           handler);
  } else if (nav.selected == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                   editServer.password, 63, InputType::Password),
                           handler);
  } else if (nav.selected == 4 && !isNewServer) {
    // Delete flow is only available for existing servers.
    if (!OPDS_STORE.removeServer(static_cast<size_t>(serverIndex))) {
      LOG_ERR("OPS", "Failed to remove OPDS server at index %d", serverIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void OpdsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int menuItems = getMenuItemCount();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (menuItems <= 0) return;
  rowValues[0] = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name;
  rowValues[1] = editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url;
  rowValues[2] = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username;
  rowValues[3] = editServer.password.empty() ? tr(STR_NOT_SET) : std::string("******");
  for (int i = 0; i < BASE_ITEMS; ++i) rowItems[i].value = rowValues[i].c_str();
  rowItems[BASE_ITEMS].value = nullptr;
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(menuItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void OpdsSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= getMenuItemCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  handleSelection();
}

void OpdsSettingsActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* header = isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, header);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight},
                    tr(STR_CALIBRE_URL_HINT));
}

void OpdsSettingsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (showSaveError) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
}
