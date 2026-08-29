#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Dedicated UTC offset picker for the status bar clock.
// Three editable fields (sign, hours, minutes); Confirm cycles fields, Up/Down adjust the active one.
// Supports the full IANA UTC offset range in 15 minute steps, including oddball zones like Nepal (+5:45).
class ClockOffsetActivity final : public Activity, private UiAppHost {
 public:
  explicit ClockOffsetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  enum Field { FIELD_SIGN = 0, FIELD_HOURS = 1, FIELD_MINUTES = 2, FIELD_COUNT };
  Field activeField = FIELD_HOURS;

  // Working copy of the offset, edited in-place. Saved back to SETTINGS on exit.
  // 0 = positive offset, 1 = negative offset.
  uint8_t sign = 0;
  // Hours: 0..14 when positive, 0..12 when negative.
  uint8_t hours = 0;
  // Quarter-hour index 0..3 (0, 15, 30, 45).
  uint8_t minutesQuarter = 0;

  void loadFromSettings();
  void saveToSettings() const;
  void adjustActiveField(int delta);
  void clampForSign();
  void getFieldRects(Rect& signRect, Rect& hoursRect, Rect& minutesRect) const;
  void getTouchControlRects(Rect& minusRect, Rect& plusRect) const;
  void buildOffsetScreen(UiScreen& screen);
  static void offsetScreen(UiScreen& screen, void* user);
  static void onFieldEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  bool routedRelease = false;
};
