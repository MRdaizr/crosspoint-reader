#include "FontDownloadActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), installer_(sdFontSystem.registry()) {}

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  refreshFamilies();
}

void FontDownloadActivity::refreshFamilies() {
  installer_.refreshRegistry();
  families_.clear();
  families_.reserve(sdFontSystem.registry().getFamilies().size());
  for (const auto& family : sdFontSystem.registry().getFamilies()) families_.push_back(family.name);
  rowValues_.assign(families_.size(), {});
  rowItems_.clear();
  rowItems_.reserve(families_.size());
  for (size_t i = 0; i < families_.size(); ++i) {
    fui::ListItem item;
    item.label = families_[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems_.push_back(item);
    rowValues_[i] = families_[i] == SETTINGS.sdFontFamilyName ? tr(STR_SELECTED) : "";
  }
  nav.selected = std::min(nav.selected, std::max(0, static_cast<int>(families_.size()) - 1));
  requestUpdate();
}

const char* FontDownloadActivity::headerTitle() const { return tr(STR_FONT_DOWNLOAD); }

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  if (rowItems_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }
  for (size_t i = 0; i < rowItems_.size(); ++i) {
    rowValues_[i] = families_[i] == SETTINGS.sdFontFamilyName ? tr(STR_SELECTED) : "";
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  syncListViewport(screen, props);
  screen.list(props);
}

void FontDownloadActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(families_.size())) return;
  app.clearTapFlash();
  confirmDelete(index);
}

void FontDownloadActivity::confirmDelete(const int index) {
  if (index < 0 || index >= static_cast<int>(families_.size())) return;
  const std::string family = families_[index];
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), family),
      [this, index](const ActivityResult& result) { handleDeleteResult(index, result); });
}

void FontDownloadActivity::handleDeleteResult(const int index, const ActivityResult& result) {
  if (result.isCancelled || index < 0 || index >= static_cast<int>(families_.size())) return;
  const auto error = installer_.deleteFamily(families_[index].c_str());
  if (error != FontInstaller::Error::OK) {
    LOG_ERR("FONT", "Failed to delete SD font family %s", families_[index].c_str());
    return;
  }
  sdFontSystem.markRegistryDirty();
  sdFontSystem.ensureLoaded(renderer);
  refreshFamilies();
}
