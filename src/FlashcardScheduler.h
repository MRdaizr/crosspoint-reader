#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

enum class FlashcardSrsState : uint8_t { LEARNING, REVIEW, RELEARNING };
enum class FlashcardGrade : uint8_t { AGAIN, HARD, GOOD };

#pragma pack(push, 1)
struct FlashcardSrsRecord {
  uint64_t cardId;
  uint32_t dueAt;
  uint16_t intervalDays;
  uint16_t easePermille;
  uint16_t repetitions;
  uint8_t lapses;
  uint8_t state;
  uint8_t step;
};
#pragma pack(pop)

class FlashcardScheduler {
  std::string statePath;
  std::vector<FlashcardSrsRecord> records;
  std::string dailyDate;
  uint16_t newCardsToday = 0;
  uint16_t reviewsToday = 0;
  uint16_t completedToday = 0;
  std::string lastStudyDate;
  uint16_t currentStreak = 0;
  uint16_t maxStreak = 0;
  uint32_t totalReviews = 0;
  bool loaded = false;

  FlashcardSrsRecord* find(uint64_t cardId);
  const FlashcardSrsRecord* find(uint64_t cardId) const;
  bool save() const;

 public:
  static constexpr uint16_t DAILY_NEW_LIMIT = 20;
  static constexpr uint16_t DAILY_REVIEW_LIMIT = 200;
  static constexpr time_t VALID_EPOCH = 1700000000;

  bool load(const std::string& deckPath);
  bool hasValidTime() const;
  uint16_t newCardsRemaining();
  uint16_t reviewCardsRemaining();
  uint16_t getCompletedToday() const { return completedToday; }
  uint16_t getCurrentStreak() const { return currentStreak; }
  uint16_t getMaxStreak() const { return maxStreak; }
  uint32_t getTotalReviews() const { return totalReviews; }
  uint16_t getNewCardsToday() const { return newCardsToday; }
  const FlashcardSrsRecord* get(uint64_t cardId) const;
  bool isDue(uint64_t cardId, time_t now) const;
  bool isLearningDue(uint64_t cardId, time_t now) const;
  bool introduce(uint64_t cardId);
  bool grade(uint64_t cardId, FlashcardGrade grade);
  uint32_t learnedCount() const;
  uint32_t dueReviewCount() const;
  uint32_t dueCountWithinDays(uint8_t days) const;
  static uint64_t cardId(const std::string& word, const std::string& phonetic, const std::string& definition);
};
