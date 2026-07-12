#pragma once

#include <ctime>

#include "activities/Activity.h"

class NtpClockActivity final : public Activity {
  enum class State : uint8_t { CONNECTING, SYNCING, DISPLAYING };

  State state = State::CONNECTING;
  bool ownsWifi = false;
  bool lastSyncFailed = false;
  time_t nextSyncAt = 0;
  unsigned long fallbackSyncAtMs = 0;
  time_t lastDisplayedMinute = 0;

  void beginSync();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void runSync();
  void scheduleNextSync();
  void tearDownWifi();
  static bool hasValidSystemTime();

 public:
  explicit NtpClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NtpClock", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;
};
