#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ExtensionsMenuActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

 public:
  explicit ExtensionsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Extensions", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
