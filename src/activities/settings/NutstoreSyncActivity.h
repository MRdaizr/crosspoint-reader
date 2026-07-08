#pragma once

#include "activities/Activity.h"
#include "network/NutstoreSync.h"

class NutstoreSyncActivity final : public Activity {
  NutstoreSyncStatus status;
  bool finished = false;
  bool success = false;
  bool cancelRequested = false;

  void runSync();
  void onWifiSelectionComplete(bool success);

 public:
  explicit NutstoreSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NutstoreSync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return !finished; }
  bool skipLoopDelay() override { return !finished; }
};
