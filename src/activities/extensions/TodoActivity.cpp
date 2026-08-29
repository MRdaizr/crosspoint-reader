#include "TodoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
std::string scheduledAtText(const TodoItem& item) {
  if (item.scheduledAt.empty()) return tr(STR_NOT_SET);
  std::string value = item.scheduledAt;
  if (value.size() > 10) value[10] = ' ';
  return value;
}
}  // namespace

bool TodoActivity::reload(const uint32_t selectedId) {
  loadFailed = !TODO_STORE.getItems(items);
  if (loadFailed) {
    items.clear();
    rowTitles.clear();
    rowDates.clear();
    rowItems.clear();
    nav.selected = 0;
    return false;
  }

  if (selectedId != 0) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].id == selectedId) {
        nav.selected = static_cast<int>(i);
        break;
      }
    }
  }
  rebuildRowItems();
  return true;
}

void TodoActivity::rebuildRowItems() {
  rowTitles.resize(items.size());
  rowDates.resize(items.size());
  rowItems.clear();
  rowItems.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    rowTitles[i] = std::string(items[i].completed ? "[x] " : "[ ] ") + items[i].title;
    rowDates[i] = scheduledAtText(items[i]);
    freeink::ui::ListItem item;
    item.label = rowTitles[i].c_str();
    item.value = rowDates[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    item.state = freeink::ui::StateNormal;
    rowItems.push_back(item);
  }
}

void TodoActivity::onEnter() {
  reload();
  UiListActivity::onEnter();
}

void TodoActivity::activateIndex(const int index) {
  if (index >= 0 && index < static_cast<int>(items.size())) {
    const uint32_t id = items[index].id;
    TodoItem changed;
    if (TODO_STORE.toggle(id, changed)) reload(id);
    else reload();
  }
  app.clearTapFlash();
  requestUpdate();
}

const char* TodoActivity::headerTitle() const { return tr(STR_TODOS); }

void TodoActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                              static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (loadFailed) {
    screen.centeredText(tr(STR_TODOS_LOAD_FAILED), screen.theme().bodyText);
  } else if (items.empty()) {
    screen.centeredText(tr(STR_NO_TODOS), screen.theme().bodyText);
  } else {
    freeink::ui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = freeink::ui::InputTouch;
    props.labelText = screen.theme().bodyText;
    props.subtitleText = screen.theme().smallText;
    syncListViewport(screen, props, true);
    screen.list(props);
  }
}
