#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "components/UiSliderDialog.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_SLIDER = 1;
constexpr fui::ActionId ACTION_STEP = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr fui::ActionId ACTION_OK = 4;
}  // namespace

IntervalSelectionActivity::IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const char* activityName, const StrId titleId,
                                                     const StrId stepHintId, const int initialValue, const int minValue,
                                                     const int maxValue, const int smallStep, const int largeStep,
                                                     const StrId valueFormatId, const bool readerActivity,
                                                     const bool ignoreInitialConfirmRelease, const StrId maxBoundaryLabelId)
    : Activity(activityName, renderer, mappedInput),
      UiAppHost(renderer),
      titleId(titleId),
      stepHintId(stepHintId),
      valueFormatId(valueFormatId),
      maxBoundaryLabelId(maxBoundaryLabelId),
      value(initialValue),
      minValue(minValue),
      maxValue(maxValue),
      smallStep(smallStep),
      largeStep(largeStep),
      readerActivity(readerActivity),
      ignoreConfirmRelease(ignoreInitialConfirmRelease) {}

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  resetUi();
  app.on(ACTION_SLIDER, &IntervalSelectionActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &IntervalSelectionActivity::onStepEvent, this);
  app.on(ACTION_CANCEL, &IntervalSelectionActivity::onCancelEvent, this);
  app.on(ACTION_OK, &IntervalSelectionActivity::onOkEvent, this);
  app.setScreen(&IntervalSelectionActivity::intervalScreen, this);
  requestUpdate();
}

void IntervalSelectionActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::setValue(const int candidate) {
  const int clamped = clampedValue(candidate);
  if (clamped == value) return;
  value = clamped;
  requestUpdate();
}

void IntervalSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void IntervalSelectionActivity::confirm() {
  setResult(IntervalResult{static_cast<uint32_t>(value)});
  finish();
}

void IntervalSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  if (event.dragPermille < 0) return;
  const int range = std::max(1, self->maxValue - self->minValue);
  self->setValue(self->minValue + (static_cast<int>(event.dragPermille) * range + 500) / 1000);
}

void IntervalSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->adjustValue(event.value * self->smallStep);
}

void IntervalSelectionActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->app.clearTapFlash();
  self->cancel();
}

void IntervalSelectionActivity::onOkEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->app.clearTapFlash();
  self->confirm();
}

void IntervalSelectionActivity::loop() {
  const auto route = routeTouch(mappedInput, false, true);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) {
    if (route.event.dragPermille >= 0) draggingSlider = true;
    return;
  }
  if (routingReady() && draggingSlider) {
    if (!route.snap.touchHeld) draggingSlider = false;
    return;
  }

  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(largeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-largeStep); });
}

void IntervalSelectionActivity::formatValue(char* buffer, const size_t size, const int forValue) const {
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && forValue == maxValue) {
    snprintf(buffer, size, "%s", I18N.get(maxBoundaryLabelId));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(buffer, size, I18N.get(valueFormatId), static_cast<unsigned int>(forValue));
  } else {
    snprintf(buffer, size, "%d", forValue);
  }
}

void IntervalSelectionActivity::intervalScreen(UiScreen& screen, void* user) {
  static_cast<IntervalSelectionActivity*>(user)->buildIntervalScreen(screen);
}

void IntervalSelectionActivity::buildIntervalScreen(UiScreen& screen) {
  char readout[64];
  formatValue(readout, sizeof(readout), value);

  char hints[2][64];
  char stepText[24];
  int hintIndex = 0;
  for (const auto& [labelId, step] :
       {std::pair{StrId::STR_STEP_HINT_FRONT, smallStep}, std::pair{StrId::STR_STEP_HINT_SIDE, largeStep}}) {
    if (valueFormatId != StrId::STR_NONE_OPT) {
      snprintf(stepText, sizeof(stepText), I18N.get(valueFormatId), static_cast<unsigned int>(step));
    } else {
      snprintf(stepText, sizeof(stepText), "%d", step);
    }
    snprintf(hints[hintIndex], sizeof(hints[hintIndex]), "%s %s", I18N.get(labelId), stepText);
    ++hintIndex;
  }

  UiSliderDialogSpec spec;
  spec.readout = readout;
  spec.value = value - minValue;
  spec.max = std::max(1, maxValue - minValue);
  spec.sliderAction = ACTION_SLIDER;
  spec.stepAction = ACTION_STEP;
  spec.cancelAction = ACTION_CANCEL;
  spec.okAction = ACTION_OK;
  // The target branch carries a page-specific hint (currently the sleep
  // timeout picker). Keep that localized sentence intact when supplied; the
  // generic two-line labels remain the fallback for future callers.
  spec.hintLine1 = stepHintId != StrId::STR_NONE_OPT ? I18N.get(stepHintId) : hints[0];
  spec.hintLine2 = stepHintId != StrId::STR_NONE_OPT ? nullptr : hints[1];
  buildSliderDialogScreen(screen, renderer, mappedInput, spec);
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(titleId), true, EpdFontFamily::BOLD);

  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
