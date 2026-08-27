#include "ExtensionsMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "FlashcardDeckListActivity.h"
#include "NtpClockActivity.h"
#include "PomodoroActivity.h"
#include "ReadingStatsActivity.h"
#include "TimerActivity.h"
#include "TodoActivity.h"
#include "activities/apps/weread/WeReadActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 7;
const StrId menuLabels[MENU_ITEMS] = {StrId::STR_FLASHCARDS, StrId::STR_READING_STATS, StrId::STR_POMODORO,
                                      StrId::STR_TIMER, StrId::STR_NTP_CLOCK, StrId::STR_TODOS, StrId::STR_WEREAD_TITLE};
const StrId menuDescs[MENU_ITEMS] = {StrId::STR_FLASHCARDS_DESC, StrId::STR_READING_STATS_DESC,
                                     StrId::STR_POMODORO_DESC, StrId::STR_TIMER_DESC, StrId::STR_NTP_CLOCK_DESC,
                                     StrId::STR_TODOS_DESC, StrId::STR_WEREAD_DESC};
const UIIcon menuIcons[MENU_ITEMS] = {UIIcon::Book, UIIcon::Recent, UIIcon::Settings, UIIcon::Settings,
                                      UIIcon::Recent, UIIcon::File, UIIcon::Book};
}  // namespace

void ExtensionsMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ExtensionsMenuActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::EXTENSIONS);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (selectedIndex) {
      case 0:
        startActivityForResult(std::make_unique<FlashcardDeckListActivity>(renderer, mappedInput), nullptr);
        break;
      case 1:
        startActivityForResult(std::make_unique<ReadingStatsActivity>(renderer, mappedInput), nullptr);
        break;
      case 2:
        startActivityForResult(std::make_unique<PomodoroActivity>(renderer, mappedInput), nullptr);
        break;
      case 3:
        startActivityForResult(std::make_unique<TimerActivity>(renderer, mappedInput), nullptr);
        break;
      case 4:
        startActivityForResult(std::make_unique<NtpClockActivity>(renderer, mappedInput), nullptr);
        break;
      case 5:
        startActivityForResult(std::make_unique<TodoActivity>(renderer, mappedInput), nullptr);
        break;
      case 6:
        startActivityForResult(std::make_unique<WeReadActivity>(renderer, mappedInput), nullptr);
        break;
      default:
        break;
    }
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    requestUpdate();
  });
}

void ExtensionsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EXTENSIONS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, selectedIndex,
               [](int index) { return std::string(I18N.get(menuLabels[index])); },
               [](int index) { return std::string(I18N.get(menuDescs[index])); },
               [](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
