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
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UiAppHelpers.h"
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
const FuiMenuIconSlot menuIconSlots[MENU_ITEMS] = {
    FuiMenuIconSlot::ExtensionsFlashcards,
    FuiMenuIconSlot::ExtensionsReadingStats,
    FuiMenuIconSlot::ExtensionsPomodoro,
    FuiMenuIconSlot::ExtensionsTimer,
    FuiMenuIconSlot::ExtensionsNtpClock,
    FuiMenuIconSlot::ExtensionsTodos,
    FuiMenuIconSlot::ExtensionsWeRead,
};
}  // namespace

void ExtensionsMenuActivity::activateIndex(const int index) {
  switch (index) {
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
}

const char* ExtensionsMenuActivity::headerTitle() const { return tr(STR_EXTENSIONS); }

void ExtensionsMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto theme = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  const bool roundedRaffLayout = theme == CrossPointSettings::UI_THEME::ROUNDEDRAFF ||
                                 theme == CrossPointSettings::UI_THEME::ROUNDEDRAFF_EXT;
  const bool roundedRaffExtLayout = theme == CrossPointSettings::UI_THEME::ROUNDEDRAFF_EXT;
  // Keep the extension menu's list band aligned with the legacy File Transfer
  // menu (NetworkModeSelectionActivity -> GUI.drawList).  GUI.drawList leaves
  // two vertical spacing units below the rows before the button hints and uses
  // the theme's content padding for both the card edge and its text inset.
  // FUI normally inherits its own tighter 8/16px insets and LightPill
  // selection style, which makes this menu visibly wider and lighter than the
  // neighbouring File Transfer menu.
  const int listBottomInset = metrics.buttonHintsHeight + (roundedRaffLayout ? metrics.verticalSpacing * 2 : 0);
  screen.setContentMargin(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                              static_cast<int16_t>(listBottomInset), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  freeink::ui::ListItem items[MENU_ITEMS]{};
  for (int i = 0; i < MENU_ITEMS; ++i) {
    items[i].label = I18N.get(menuLabels[i]);
    // File Transfer's legacy GUI.drawList renders its descriptions below the
    // title.  Use FUI's subtitle slot for the same two-line row instead of the
    // trailing value slot, which would place the description on the right.
    items[i].subtitle = I18N.get(menuDescs[i]);
    items[i].icon = GUI.showsFuiMenuIcon(menuIconSlots[i]) ? listIconFor(menuIcons[i], 32)
                                                           : freeink::ui::BitmapRef{};
    items[i].actionValue = static_cast<int16_t>(i);
  }
  freeink::ui::ListProps props;
  props.items = items;
  props.count = MENU_ITEMS;
  props.action = ACTION_ROW;
  props.inputMask = freeink::ui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.valueInset = 8;
  if (roundedRaffExtLayout) {
    // RoundedRaffExtTheme's legacy drawList intentionally uses UI_12 for
    // extension subtitles.  Bind the same font in FUI instead of its default
    // small-text slot so the title/description block has the same line rhythm.
    props.subtitleText = screen.theme().bodyText;
    props.subtitleText.bold = false;
  }
  if (roundedRaffLayout) {
    props.rowInset = static_cast<int16_t>(metrics.contentSidePadding);
    props.sidePadding = static_cast<int16_t>(metrics.contentSidePadding);
    props.rowRadius = 20;
    props.scrollIndicatorInset = static_cast<int16_t>(metrics.scrollBarRightOffset);
    // RoundedRaff's GUI list inverts the selected card (black background,
    // white text).  Supplying explicit styles bypasses the FUI LightPill theme
    // token while leaving other FUI themes unchanged.
    props.rowStyles = freeink::ui::defaultListRowStyles();
  }
  syncListViewport(screen, props, true);
  screen.list(props);
}
