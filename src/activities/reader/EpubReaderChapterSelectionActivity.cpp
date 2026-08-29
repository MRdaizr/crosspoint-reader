#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub ? epub->getTocItemsCount() : 0; }

void EpubReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  if (!epub) {
    return;
  }

  nav.selected = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (nav.selected < 0) nav.selected = 0;

  sdFontSystem.ensureLoaded(renderer);
}

void EpubReaderChapterSelectionActivity::activateIndex(const int index) {
  if (!epub || index < 0 || index >= getTotalItems()) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto tocItem = epub->getTocItem(index);
  if (tocItem.spineIndex == -1) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
  }
  finish();
}

void EpubReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const int count = getTotalItems();
  if (count <= 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }
  rowLabels.clear();
  rowItems.clear();
  rowLabels.reserve(static_cast<size_t>(count));
  rowItems.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const auto itemData = epub->getTocItem(i);
    const int indent = itemData.level > 0 ? static_cast<int>(itemData.level - 1) * 2 : 0;
    rowLabels.emplace_back(std::string(static_cast<size_t>(indent), ' ') +
                           (itemData.title.empty() ? tr(STR_UNNAMED) : itemData.title));
    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems.push_back(item);
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
