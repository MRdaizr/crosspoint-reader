#pragma once

#include <functional>

#include "activities/Activity.h"

enum class ClearCacheType { Reading, Flashcards, ReadingStats, PomodoroStats };

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              ClearCacheType cacheType = ClearCacheType::Reading)
      : Activity("ClearCache", renderer, mappedInput), cacheType(cacheType) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  ClearCacheType cacheType;
  void clearCache();
};
