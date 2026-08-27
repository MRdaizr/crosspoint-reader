#pragma once

#include <cstdint>

#include "activities/Activity.h"

class TimerActivity final : public Activity {
  enum class Mode : uint8_t { COUNTDOWN, STOPWATCH };
  enum class State : uint8_t { SETUP, RUNNING, PAUSED, COMPLETION };

  inline static constexpr unsigned long PRESET_MINUTES[] = {1UL, 5UL, 10UL, 15UL, 30UL, 60UL};
  static constexpr uint8_t PRESET_COUNT = sizeof(PRESET_MINUTES) / sizeof(PRESET_MINUTES[0]);

  Mode mode = Mode::COUNTDOWN;
  State state = State::SETUP;
  uint8_t presetIndex = 3;
  unsigned long remainingMs = 0;
  unsigned long elapsedMs = 0;
  unsigned long completionElapsedMs = 0;
  unsigned long lastTickMs = 0;

  unsigned long selectedDurationMs() const;
  const char* modeLabel() const;
  void changePreset(int delta);
  void startTimer();
  void resetTimer();
  void completeTimer();
  bool advanceTimer(unsigned long now);
  unsigned long displayedMs() const;
  void renderSetup();
  void renderRunning();
  void renderCompletion();
  void drawTimeValue(unsigned long timeMs);

 public:
  explicit TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Timer", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool allowPowerSaving() override { return true; }
};
