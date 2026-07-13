#pragma once

#include "activities/Activity.h"

class PomodoroActivity final : public Activity {
  enum class Phase { FOCUS, SHORT_BREAK, LONG_BREAK, COMPLETION };

  static constexpr unsigned long FOCUS_DURATION_MS = 25UL * 60UL * 1000UL;
  static constexpr unsigned long SHORT_BREAK_DURATION_MS = 5UL * 60UL * 1000UL;
  static constexpr unsigned long LONG_BREAK_DURATION_MS = 15UL * 60UL * 1000UL;
  static constexpr unsigned long COMPLETION_NOTICE_MS = 3000UL;

  Phase phase = Phase::FOCUS;
  Phase nextPhase = Phase::FOCUS;
  unsigned long remainingMs = FOCUS_DURATION_MS;
  unsigned long lastTickMs = 0;
  unsigned long completionEndsAtMs = 0;
  uint8_t completedFocusesInCycle = 0;
  bool running = false;
  bool showingStats = false;

  void startPhase(Phase newPhase, bool startImmediately);
  void completeCurrentPhase();
  const char* phaseLabel() const;
  const char* completionLabel() const;
  void renderTimer();
  void renderCompletion();
  void renderStats();

 public:
  explicit PomodoroActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pomodoro", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;
};
