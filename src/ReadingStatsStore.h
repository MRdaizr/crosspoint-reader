#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

inline uint64_t getDailyReadingGoalMs() { return SETTINGS.getDailyGoalMs(); }

struct ReadingDayStats {
  uint32_t dayOrdinal = 0;
  uint64_t readingMs = 0;
};

struct ReadingBookStats {
  std::string bookId;
  std::string path;
  std::vector<std::string> knownPaths;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  std::string chapterTitle;
  std::vector<ReadingDayStats> readingDays;
  uint64_t totalReadingMs = 0;
  uint32_t sessions = 0;
  uint32_t lastSessionMs = 0;
  uint32_t firstReadAt = 0;
  uint32_t lastReadAt = 0;
  uint32_t completedAt = 0;
  uint8_t lastProgressPercent = 0;
  uint8_t chapterProgressPercent = 0;
  bool completed = false;
};

struct ReadingSessionSnapshot {
  bool valid = false;
  uint32_t serial = 0;
  std::string bookId;
  std::string path;
  uint32_t sessionMs = 0;
  bool counted = false;
  bool completedThisSession = false;
  uint8_t startProgressPercent = 0;
  uint8_t endProgressPercent = 0;
};

struct ReadingSessionLogEntry {
  uint32_t dayOrdinal = 0;
  uint32_t sessionMs = 0;
};

// Per-book, per-day reading statistics. Sessions are credited from the
// monotonic millis() clock and bucketed by the configured local calendar day.
class ReadingStatsStore {
  static ReadingStatsStore instance;

  struct ActiveSession {
    bool active = false;
    size_t bookIndex = 0;
    unsigned long lastActivityMs = 0;
    uint64_t accumulatedMs = 0;
    uint8_t startProgressPercent = 0;
    bool startCompleted = false;
  };

  std::vector<ReadingBookStats> books;
  std::vector<ReadingDayStats> legacyReadingDays;
  std::vector<ReadingDayStats> readingDays;
  std::vector<ReadingSessionLogEntry> sessionLog;
  ActiveSession activeSession;
  ReadingSessionSnapshot lastSessionSnapshot;
  uint32_t sessionSerial = 0;
  mutable bool loaded = false;
  mutable bool dirty = false;
  mutable unsigned long dirtySinceMs = 0;
  mutable unsigned long lastSaveAttemptMs = 0;

  size_t findBookIndexByPath(const std::string& path) const;
  size_t findBookIndexById(const std::string& id) const;
  size_t createOrFindBook(const std::string& path, const std::string& title, const std::string& author,
                          const std::string& coverBmpPath);
  void normalize();
  void normalizeDays(std::vector<ReadingDayStats>& days);
  void rebuildAggregatedDays();
  void markDirty();
  void creditElapsed();
  void recordReadingTime(ReadingBookStats& book, uint32_t timestamp, uint64_t milliseconds);
  void appendSessionLog(uint32_t dayOrdinal, uint32_t sessionMs);
  uint32_t latestKnownTimestamp() const;
  uint32_t referenceTimestamp(uint32_t preferred = 0, uint32_t bookTimestamp = 0) const;
  static bool isClockValid(uint32_t timestamp);
  void ensureLoaded() const;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void beginSession(const std::string& path, const std::string& title, const std::string& author = {},
                    const std::string& coverBmpPath = {}, uint8_t progressPercent = 0,
                    const std::string& chapterTitle = {}, uint8_t chapterProgressPercent = 0);
  void noteActivity();
  void tickActiveSession();
  void resumeSession();
  void updateProgress(uint8_t progressPercent, bool completed = false, const std::string& chapterTitle = {},
                      uint8_t chapterProgressPercent = 0);
  void endSession();
  void addSession(const std::string& path, const std::string& title, uint32_t seconds);

  bool adjustBookReadingTime(const std::string& path, uint32_t dayOrdinal, int32_t deltaMs);
  bool updateBookMetadata(const std::string& path, const std::string& title, const std::string& author,
                          const std::string& coverBmpPath);
  bool updateBookPath(const std::string& oldKey, const std::string& newPath, const std::string& title = {},
                      const std::string& author = {}, const std::string& coverBmpPath = {},
                      const std::string& bookId = {});
  bool updateBookPathPrefix(const std::string& oldPrefix, const std::string& newPrefix);
  bool removeBook(const std::string& path);
  const ReadingBookStats* findBook(const std::string& key) const;
  const ReadingBookStats* findMatchingBookForPath(const std::string& path, const std::string& title = {},
                                                  const std::string& author = {}) const;
  const ReadingSessionSnapshot& getLastSessionSnapshot() const { return lastSessionSnapshot; }

  const std::vector<ReadingBookStats>& getBooks() const {
    ensureLoaded();
    return books;
  }
  const std::vector<ReadingDayStats>& getReadingDays() const {
    ensureLoaded();
    return readingDays;
  }
  const std::vector<ReadingSessionLogEntry>& getSessionLog() const {
    ensureLoaded();
    return sessionLog;
  }
  static bool shouldIgnorePath(const std::string& path);

  uint32_t getBooksStartedCount() const {
    ensureLoaded();
    return static_cast<uint32_t>(books.size());
  }
  uint32_t getBooksFinishedCount() const;
  uint64_t getTotalReadingMs() const;
  uint64_t getTodayReadingMs() const;
  uint64_t getRecentReadingMs(uint32_t days) const;
  uint32_t getCurrentStreakDays() const;
  uint32_t getMaxStreakDays() const;
  uint32_t getReferenceDayOrdinal() const;
  uint32_t getDisplayTimestamp(bool* usedFallback = nullptr) const;
  bool hasReadingDays() const {
    ensureLoaded();
    return !readingDays.empty();
  }

  void reset();
  void clear();
  bool exportToFile(const std::string& path) const;
  bool importFromFile(const std::string& path);
  bool shouldSaveCheckpoint() const;
  bool saveToFile() const;
  bool loadFromFile();

  struct LegacyEntry {
    std::string path;
    std::string title;
    uint32_t totalSeconds = 0;
  };
  const std::vector<LegacyEntry>& getEntries() const;

 private:
  mutable std::vector<LegacyEntry> legacyEntries;
};

#define READING_STATS ReadingStatsStore::getInstance()
