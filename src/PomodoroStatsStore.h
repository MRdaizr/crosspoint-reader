#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PomodoroDailyEntry {
  std::string date;
  uint32_t completedFocuses = 0;
  uint32_t focusSeconds = 0;
};

class PomodoroStatsStore {
  static PomodoroStatsStore instance;

  bool loaded = false;
  uint32_t totalCompletedFocuses = 0;
  uint32_t totalFocusSeconds = 0;
  std::vector<PomodoroDailyEntry> dailyEntries;

 public:
  static PomodoroStatsStore& getInstance() { return instance; }

  void loadFromFile();
  bool saveToFile() const;
  void clear();
  void recordCompletedFocus(uint32_t seconds);
  bool hasValidDate() const;
  uint32_t getTotalCompletedFocuses();
  uint32_t getTotalFocusSeconds();
  const std::vector<PomodoroDailyEntry>& getRecentDailyEntries();
};

#define POMODORO_STATS PomodoroStatsStore::getInstance()
