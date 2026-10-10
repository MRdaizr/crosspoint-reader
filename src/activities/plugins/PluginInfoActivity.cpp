#include "PluginInfoActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/CatalogScreens.h"
#include "util/PluginEvents.h"

void PluginInfoActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(
      ACTION_USER,
      [](const freeink::ui::ActionEvent&, void* user) { static_cast<PluginInfoActivity*>(user)->onBackButton(); },
      this);
  refresh();
}
void PluginInfoActivity::refresh() {
  permission = PluginPermissions::inspect(plugin.name.c_str());
  message = permission.approved ? tr(STR_PLUGIN_ENABLED)
                                : (permission.enabled ? tr(STR_PLUGIN_REAPPROVE) : tr(STR_PLUGIN_DISABLED));
  count = (permission.available ? 1 : 0) + (permission.enabled && !permission.approved ? 1 : 0) +
          (plugin.deviceKind == PluginLocations::DeviceKind::Catalog ? 1 : 0) + (!plugin.readmePath.empty() ? 1 : 0);
  nav.reset();
  requestUpdate();
}
void PluginInfoActivity::buildScreen(UiScreen& screen) {
  catalogScreenHeader(screen, renderer, plugin.title.c_str(), {}, freeink::ui::NO_ACTION, ACTION_USER);
  auto style = screen.theme().bodyText;
  style.maxLines = 8;
  const int available = confirming ? screen.body().height - 2 * screen.theme().rowHeight - screen.theme().spaceMd
                                   : screen.body().height / 2;
  const int16_t textHeight =
      std::max<int16_t>(0, std::min<int>(available, screen.target().lineHeight(style.font) * (confirming ? 8 : 4)));
  screen.target().text(screen.takeTop(textHeight, screen.theme().spaceMd),
                       confirming ? tr(STR_PLUGIN_TRUST_WARNING) : message.c_str(), style);
  if (!confirming && !plugin.description.empty()) {
    style.maxLines = 2;
    screen.target().text(screen.takeTop(screen.target().lineHeight(style.font) * 2, screen.theme().spaceMd),
                         plugin.description.c_str(), style);
  }
  int n = 0;
  const auto add = [&](const char* label) {
    rows[n] = {};
    rows[n].label = label;
    rows[n].actionValue = n;
    ++n;
  };
  if (confirming) {
    add(tr(STR_CANCEL));
    add(tr(STR_PLUGIN_ENABLE));
  } else {
    if (permission.available) add(permission.approved ? tr(STR_PLUGIN_DISABLE) : tr(STR_PLUGIN_ENABLE));
    if (permission.enabled && !permission.approved) add(tr(STR_PLUGIN_DISABLE));
    if (plugin.deviceKind == PluginLocations::DeviceKind::Catalog) add(tr(STR_OPEN));
    if (!plugin.readmePath.empty()) add(tr(STR_PLUGIN_README));
  }
  count = n;
  freeink::ui::ListProps props;
  props.items = rows;
  props.count = count;
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}
void PluginInfoActivity::openCatalog() {
  if (!PluginPermissions::allowed(plugin.name.c_str())) return;
  setResult(ActivityResult(FilePathResult{plugin.manifestPath}));
  finish();
}
void PluginInfoActivity::activateIndex(int index) {
  if (index < 0 || index >= count) return;
  app.clearTapFlash();
  if (confirming) {
    if (index == 1) {
      if (!PluginPermissions::setEnabled(plugin.name.c_str(), true, permission.digest)) {
        confirming = false;
        refresh();
        message = tr(STR_PLUGIN_REAPPROVE);
        return;
      }
      pluginevents::refreshSubscriptions();
      if (openAfterApproval) {
        openCatalog();
        return;
      }
    }
    confirming = false;
    refresh();
    return;
  }
  if (permission.available && index == 0) {
    if (permission.approved) {
      PluginPermissions::setEnabled(plugin.name.c_str(), false);
      pluginevents::refreshSubscriptions();
      refresh();
    } else {
      confirming = true;
      openAfterApproval = false;
      count = 2;
      nav.reset();
      requestUpdate();
    }
    return;
  }
  index -= permission.available ? 1 : 0;
  if (permission.enabled && !permission.approved) {
    if (index == 0) {
      PluginPermissions::setEnabled(plugin.name.c_str(), false);
      pluginevents::refreshSubscriptions();
      refresh();
      return;
    }
    --index;
  }
  if (plugin.deviceKind == PluginLocations::DeviceKind::Catalog) {
    if (index == 0) {
      if (PluginPermissions::allowed(plugin.name.c_str()))
        openCatalog();
      else {
        confirming = true;
        openAfterApproval = true;
        count = 2;
        nav.reset();
        requestUpdate();
      }
      return;
    }
    --index;
  }
  if (index == 0 && !plugin.readmePath.empty()) {
    std::string text;
    if (!pluginhttp::readFile(plugin.readmePath, 16 * 1024, text)) return;
    auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, plugin.title, std::move(text));
    if (!viewer) {
      LOG_ERR("PINF", "OOM: README");
      return;
    }
    startActivityForResult(std::move(viewer), [](const ActivityResult&) {});
  }
}
void PluginInfoActivity::onBackButton() {
  if (confirming) {
    confirming = false;
    refresh();
    return;
  }
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
