#pragma once

#include <functional>
#include <string>

#include "activities/UiListActivity.h"

class ButtonRemapActivity final : public UiListActivity {
 public:
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ButtonRemap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  // Rendering task state.

  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;

  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  const char* getRoleName(uint8_t roleIndex) const;
  const char* getHardwareName(uint8_t buttonIndex) const;

  int listCount() const override { return 4; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;
};
