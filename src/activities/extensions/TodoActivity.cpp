#include "TodoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

bool TodoActivity::reload(const uint32_t selectedId) {
  loadFailed = !TODO_STORE.getItems(items);
  if (loadFailed) {
    items.clear();
    selectedIndex = 0;
    return false;
  }

  selectedIndex = 0;
  if (selectedId != 0) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].id == selectedId) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
  }
  return true;
}

void TodoActivity::onEnter() {
  Activity::onEnter();
  reload();
  requestUpdate(true);
}

void TodoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int itemCount = static_cast<int>(items.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && itemCount > 0) {
    const uint32_t id = items[selectedIndex].id;
    TodoItem changed;
    if (TODO_STORE.toggle(id, changed)) reload(id);
    else reload();
    requestUpdate();
    return;
  }

  if (itemCount <= 0) return;
  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void TodoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TODOS));

  if (loadFailed) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_TODOS_LOAD_FAILED));
  } else if (items.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_TODOS));
  } else {
    std::string allTitles;
    for (const auto& item : items) {
      allTitles += item.title;
      allTitles += '\n';
    }
    const int fontId = DynamicFont::fontForCjkText(renderer, allTitles.c_str(), 0);
    DynamicFont::prewarmIfSdFont(renderer, fontId, allTitles);
    const int pageItems = std::max(1, GUI.getListPageItems(contentHeight, false));
    const int rowHeight = metrics.listRowHeight;
    const int rowStep = GUI.getListRowStep(false);
    const int titleFontId = fontId != 0 ? fontId : UI_10_FONT_ID;
    const int titleOffsetY = GUI.getListTitleOffsetY(renderer, rowHeight, titleFontId);
    const int first = selectedIndex / pageItems * pageItems;

    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(items.size()), selectedIndex,
                 [this](int index) { return std::string("    ") + items[index].title; }, nullptr, nullptr, nullptr,
                 false, [this](int index) { return items[index].completed; }, fontId);

    for (int index = first; index < static_cast<int>(items.size()) && index < first + pageItems; ++index) {
      const int rowY = contentTop + (index - first) * rowStep;
      const bool selected = index == selectedIndex;
      const int boxSize = 13;
      const int boxX = metrics.contentSidePadding;
      const int boxY = rowY + titleOffsetY + (renderer.getLineHeight(titleFontId) - boxSize) / 2;
      if (items[index].completed) renderer.fillRect(boxX, boxY, boxSize, boxSize, !selected);
      else renderer.drawRect(boxX, boxY, boxSize, boxSize, !selected);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
