#pragma once

#include "activities/Activity.h"

class PomodoroActivity final : public Activity {
  static constexpr unsigned long DEFAULT_DURATION_MS = 25UL * 60UL * 1000UL;
  unsigned long durationMs = DEFAULT_DURATION_MS;
  unsigned long remainingMs = DEFAULT_DURATION_MS;
  unsigned long lastTickMs = 0;
  bool running = false;

 public:
  explicit PomodoroActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pomodoro", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  void render(RenderLock&&) override;
};
