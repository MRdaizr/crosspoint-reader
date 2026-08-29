#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public Activity, private UiAppHost {
 public:
  // Slider-style percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              int initialPercent);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Current percent value (0-100) shown on the slider.
  int percent = 0;

  ButtonNavigator buttonNavigator;
  bool draggingSlider = false;

  // Change the current percent by a delta and clamp within bounds.
  void adjustPercent(int delta);
  void setPercent(int value);
  void cancel();
  void confirm();
  static void percentScreen(UiScreen& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onOkEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildPercentScreen(UiScreen& screen);
};
