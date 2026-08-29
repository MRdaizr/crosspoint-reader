#pragma once

#include <I18n.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

class IntervalSelectionActivity final : public Activity, private UiAppHost {
 public:
  explicit IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName,
                                     StrId titleId, StrId stepHintId, int initialValue, int minValue, int maxValue,
                                     int smallStep, int largeStep, StrId valueFormatId = StrId::STR_NONE_OPT,
                                     bool readerActivity = false, bool ignoreInitialConfirmRelease = false,
                                     StrId maxBoundaryLabelId = StrId::STR_NONE_OPT);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity; }

 private:
  StrId titleId;
  StrId stepHintId;
  StrId valueFormatId;
  StrId maxBoundaryLabelId;
  int value;
  int minValue;
  int maxValue;
  int smallStep;
  int largeStep;
  bool readerActivity;
  bool ignoreConfirmRelease;
  ButtonNavigator buttonNavigator;
  bool draggingSlider = false;

  void adjustValue(int delta);
  int clampedValue(int candidate) const;
  void setValue(int candidate);
  void formatValue(char* buffer, size_t size, int forValue) const;
  void cancel();
  void confirm();
  static void intervalScreen(UiScreen& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onOkEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildIntervalScreen(UiScreen& screen);
};
