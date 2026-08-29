#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  if (!xtc) {
    return;
  }

  nav.selected = findChapterIndexForPage(currentPage);

  sdFontSystem.ensureLoaded(renderer);

}

void XtcReaderChapterSelectionActivity::activateIndex(const int index) {
  if (!xtc || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  setResult(PageResult{xtc->getChapters()[static_cast<size_t>(index)].startPage});
  finish();
}

void XtcReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (!xtc || xtc->getChapters().empty()) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }
  rowLabels.clear(); rowItems.clear();
  rowLabels.reserve(xtc->getChapters().size()); rowItems.reserve(xtc->getChapters().size());
  for (size_t i = 0; i < xtc->getChapters().size(); ++i) {
    const auto& chapter = xtc->getChapters()[i];
    rowLabels.push_back(chapter.name.empty() ? std::string(tr(STR_UNNAMED)) : chapter.name);
    fui::ListItem item; item.label = rowLabels.back().c_str(); item.actionValue = static_cast<int16_t>(i); item.icon = {};
    rowItems.push_back(item);
  }
  fui::ListProps props; props.items = rowItems.data(); props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW; props.inputMask = fui::InputTouch; props.labelText = screen.theme().smallText; props.labelText.maxLines = 2;
  syncListViewport(screen, props); screen.list(props);
}
