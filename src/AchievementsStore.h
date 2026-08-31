#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"

enum class AchievementMetric : uint8_t {
  BooksStarted,
  BooksFinished,
  Sessions,
  TotalReadingMs,
  GoalDays,
  MaxGoalStreak,
  MaxSessionMs,
};

struct AchievementDefinition {
  AchievementMetric metric;
  uint64_t target;
  const char* title;
};

struct AchievementState {
  bool unlocked = false;
  uint32_t unlockedAt = 0;
};

struct AchievementView {
  const AchievementDefinition* definition = nullptr;
  AchievementState state;
  uint64_t progress = 0;
};

// Small persistent achievement ledger built on top of ReadingStatsStore.
// Definitions are intentionally data-driven so new milestones do not change
// the on-disk state layout.
class AchievementsStore {
  static AchievementsStore instance;
  std::vector<AchievementState> states;
  std::vector<size_t> pendingUnlocks;
  uint32_t longestSessionMs = 0;
  bool loaded = false;
  mutable bool dirty = false;

  static const std::vector<AchievementDefinition>& definitions();
  static uint32_t referenceTimestamp();
  uint64_t progressFor(AchievementMetric metric) const;
  void ensureLoaded() const;

 public:
  static AchievementsStore& getInstance() { return instance; }

  bool loadFromFile();
  bool saveToFile() const;
  void reset();
  void clear();
  void reconcileFromCurrentStats(bool persist = true, bool enqueuePopups = false);
  void recordSessionEnded(const ReadingSessionSnapshot& snapshot);
  bool hasPendingUnlocks() const { return !pendingUnlocks.empty(); }
  void clearPendingUnlocks() { pendingUnlocks.clear(); }
  std::string popNextPopupMessage();
  std::vector<AchievementView> buildViews() const;
  size_t unlockedCount() const;
};

#define ACHIEVEMENTS AchievementsStore::getInstance()
