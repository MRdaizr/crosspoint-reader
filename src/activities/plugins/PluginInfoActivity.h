#pragma once
#include "PluginCatalogActivity.h"
#include "util/PluginPermissions.h"

// Every picker row opens this information page. Viewing a README is safe while
// disabled; network operations/catalogs require an explicit trust confirmation.
class PluginInfoActivity final : public UiListActivity {
 public:
  PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& input, const PluginRef& plugin)
      : UiListActivity("PluginInfo", renderer, input), plugin(plugin) {}
  void onEnter() override;

 private:
  PluginRef plugin;
  PluginPermissions::Status permission;
  freeink::ui::ListItem rows[4];
  int count = 0;
  bool confirming = false, openAfterApproval = false;
  std::string message;
  int listCount() const override { return count; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void refresh();
  void openCatalog();
};
