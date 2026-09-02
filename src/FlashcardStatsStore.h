#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FlashcardScheduler.h"

struct FlashcardDailyStats {
  std::string date;
  uint32_t completed = 0;
  uint32_t newCards = 0;
  uint32_t learningReviews = 0;
  uint32_t reviewReviews = 0;
  uint32_t again = 0;
  uint32_t hard = 0;
  uint32_t good = 0;
};

class FlashcardStatsStore {
  static FlashcardStatsStore instance;

  bool loaded = false;
  std::vector<FlashcardDailyStats> dailyEntries;

  void sortAndTrim();
  FlashcardDailyStats* todayEntry();

 public:
  static FlashcardStatsStore& getInstance() { return instance; }

  void loadFromFile();
  bool saveToFile() const;
  void clear();
  void recordNewCard();
  void recordReview(FlashcardGrade grade, bool reviewCard);
  const std::vector<FlashcardDailyStats>& getRecentDailyEntries();
};

#define FLASHCARD_STATS FlashcardStatsStore::getInstance()
