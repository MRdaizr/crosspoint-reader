#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// UI steps correspond to logical roles in order: Back, Confirm, Left, Right.
constexpr uint8_t kRoleCount = 4;
// Marker used when a role has not been assigned yet.
constexpr uint8_t kUnassigned = 0xFF;
// Duration to show temporary error text when reassigning a button.
constexpr unsigned long kErrorDisplayMs = 1500;
}  // namespace

void ButtonRemapActivity::onEnter() {
  UiListActivity::onEnter();

  // Start with all roles unassigned to avoid duplicate blocking.
  currentStep = 0;
  tempMapping[0] = kUnassigned;
  tempMapping[1] = kUnassigned;
  tempMapping[2] = kUnassigned;
  tempMapping[3] = kUnassigned;
  errorMessage.clear();
  errorUntil = 0;
  nav.selected = 0;
  requestUpdate();
}

void ButtonRemapActivity::onExit() { UiListActivity::onExit(); }

bool ButtonRemapActivity::handleCustomInput() {
  // Clear any temporary warning after its timeout.
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return true;
  }

  // Side buttons:
  // - Up: reset mapping to defaults and exit.
  // - Down: cancel without saving.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Persist default mapping immediately so the user can recover quickly.
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    SETTINGS.saveToFile();
    finish();
    return true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    // Exit without changing settings.
    finish();
    return true;
  }

  {
    // Make sure UI done rendering before accepting another assignment.
    // This avoids rapid double-presses that can advance the step without a visible redraw.
    RenderLock lock(*this);

    // Wait for a front button press to assign to the current role.
    const int pressedButton = mappedInput.getPressedFrontButton();
    if (pressedButton < 0) return false;

    // Update temporary mapping and advance the remap step.
    // Only accept the press if this hardware button isn't already assigned elsewhere.
    if (!validateUnassigned(static_cast<uint8_t>(pressedButton))) {
      requestUpdate();
      return true;
    }
    tempMapping[currentStep] = static_cast<uint8_t>(pressedButton);
    currentStep++;
    nav.selected = std::min<int>(currentStep, kRoleCount - 1);

    if (currentStep >= kRoleCount) {
      // All roles assigned; save to settings and exit.
      applyTempMapping();
      SETTINGS.saveToFile();
      finish();
      return true;
    }

    requestUpdate();
  }
  return true;
}

bool ButtonRemapActivity::handleButtons() {
  // Front-button presses are captured by handleCustomInput(). Keep Back as an
  // escape path for users who do not want to complete the assignment.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  return false;
}

void ButtonRemapActivity::activateIndex(const int index) {
  // Touch selection is intentionally disabled for this screen: assigning a
  // role must always come from a raw physical front-button press.
  (void)index;
}

const char* ButtonRemapActivity::headerTitle() const { return tr(STR_REMAP_FRONT_BUTTONS); }

void ButtonRemapActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                              static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  const int16_t promptHeight = static_cast<int16_t>(metrics.tabBarHeight);
  const auto prompt = screen.takeTop(promptHeight);
  screen.target().text(prompt, tr(STR_REMAP_PROMPT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Reserve help/error lines below the rows without registering them as
  // actions. Role assignment is driven by raw front-button capture.
  const int16_t smallLine = screen.target().lineHeight(screen.theme().smallText.font);
  const int16_t helpHeight = static_cast<int16_t>(smallLine * 2 + metrics.verticalSpacing);
  const auto help = screen.takeBottom(helpHeight);
  screen.target().text(help, tr(STR_REMAP_RESET_HINT), screen.theme().smallText);
  screen.target().text(freeink::ui::Rect{help.x, static_cast<int16_t>(help.y + smallLine), help.width, smallLine},
                       tr(STR_REMAP_CANCEL_HINT), screen.theme().smallText);

  if (!errorMessage.empty()) {
    const auto error = screen.takeBottom(static_cast<int16_t>(smallLine + metrics.verticalSpacing));
    screen.target().text(error, errorMessage.c_str(), screen.theme().smallText);
  }

  freeink::ui::ListItem rows[4]{};
  for (int i = 0; i < 4; ++i) {
    rows[i].label = getRoleName(static_cast<uint8_t>(i));
    const uint8_t assigned = tempMapping[i];
    rows[i].value = assigned == kUnassigned ? tr(STR_UNASSIGNED) : getHardwareName(assigned);
    rows[i].actionValue = static_cast<int16_t>(i);
  }
  freeink::ui::ListProps props;
  props.items = rows;
  props.count = 4;
  // No touch activation: a role is assigned only by a raw front-button press.
  props.inputMask = freeink::ui::InputNone;
  props.labelText = screen.theme().bodyText;
  props.valueText = screen.theme().smallText;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void ButtonRemapActivity::drawFooter() {
  const auto labelForHardware = [&](const uint8_t hardwareIndex) -> const char* {
    for (uint8_t i = 0; i < kRoleCount; ++i) {
      if (tempMapping[i] == hardwareIndex) return getRoleName(i);
    }
    return "-";
  };
  GUI.drawButtonHints(renderer, labelForHardware(CrossPointSettings::FRONT_HW_BACK),
                      labelForHardware(CrossPointSettings::FRONT_HW_CONFIRM),
                      labelForHardware(CrossPointSettings::FRONT_HW_LEFT),
                      labelForHardware(CrossPointSettings::FRONT_HW_RIGHT));
}

void ButtonRemapActivity::applyTempMapping() {
  // Commit temporary mapping into settings (logical role -> hardware).
  SETTINGS.frontButtonBack = tempMapping[0];
  SETTINGS.frontButtonConfirm = tempMapping[1];
  SETTINGS.frontButtonLeft = tempMapping[2];
  SETTINGS.frontButtonRight = tempMapping[3];
}

bool ButtonRemapActivity::validateUnassigned(const uint8_t pressedButton) {
  // Block reusing a hardware button already assigned to another role.
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == pressedButton && i != currentStep) {
      errorMessage = tr(STR_ALREADY_ASSIGNED);
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* ButtonRemapActivity::getRoleName(const uint8_t roleIndex) const {
  switch (roleIndex) {
    case 0:
      return tr(STR_BACK);
    case 1:
      return tr(STR_CONFIRM);
    case 2:
      return tr(STR_DIR_LEFT);
    case 3:
    default:
      return tr(STR_DIR_RIGHT);
  }
}

const char* ButtonRemapActivity::getHardwareName(const uint8_t buttonIndex) const {
  switch (buttonIndex) {
    case CrossPointSettings::FRONT_HW_BACK:
      return tr(STR_HW_BACK_LABEL);
    case CrossPointSettings::FRONT_HW_CONFIRM:
      return tr(STR_HW_CONFIRM_LABEL);
    case CrossPointSettings::FRONT_HW_LEFT:
      return tr(STR_HW_LEFT_LABEL);
    case CrossPointSettings::FRONT_HW_RIGHT:
      return tr(STR_HW_RIGHT_LABEL);
    default:
      return "Unknown";
  }
}
