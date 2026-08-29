#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

void EpubReaderFootnotesActivity::onEnter() {
  UiListActivity::onEnter();
  rowLabels.clear();
  rowSubtitles.clear();
  rowItems.clear();
  rowLabels.reserve(footnotes.size());
  rowSubtitles.reserve(footnotes.size());
  rowItems.reserve(footnotes.size());
  for (const auto& footnote : footnotes) {
    rowLabels.push_back(footnote.number[0] == '\0' ? std::string(tr(STR_LINK)) : footnote.number);
    rowSubtitles.push_back(footnote.href);
    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    item.subtitle = rowSubtitles.back().empty() ? nullptr : rowSubtitles.back().c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    item.icon = {};
    rowItems.push_back(item);
  }
}

bool EpubReaderFootnotesActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) && !footnotes.empty()) {
    activateIndex(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void EpubReaderFootnotesActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(footnotes.size())) return;
  app.clearTapFlash();
  nav.selected = index;
  setResult(FootnoteResult{footnotes[static_cast<size_t>(index)].href});
  finish();
}

void EpubReaderFootnotesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_FOOTNOTES), screen.theme().bodyText);
    return;
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

void EpubReaderFootnotesActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), footnotes.empty() ? "" : tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
