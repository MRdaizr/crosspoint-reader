#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ReadingStatEntry {
  std::string path;
  std::string title;
  uint32_t totalSeconds = 0;
};

class ReadingStatsStore {
  static ReadingStatsStore instance;

  std::vector<ReadingStatEntry> entries;
  bool loaded = false;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void loadFromFile();
  bool saveToFile() const;
  void addSession(const std::string& path, const std::string& title, uint32_t seconds);
  const std::vector<ReadingStatEntry>& getEntries();
};

#define READING_STATS ReadingStatsStore::getInstance()
