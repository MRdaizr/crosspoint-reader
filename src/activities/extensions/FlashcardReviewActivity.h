#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FlashcardScheduler.h"
#include "activities/Activity.h"

class HalFile;

struct FlashcardCard {
  std::string word;
  std::string phonetic;
  std::string definition;
};

class FlashcardReviewActivity final : public Activity {
  std::string deckPath;
  std::string indexPath;
  FlashcardCard currentCard;
  uint32_t cardCount = 0;
  uint8_t wordColumn = 0;
  uint8_t phoneticColumn = 1;
  uint8_t definitionColumn = 2;
  int selectedIndex = 0;
  uint64_t currentCardId = 0;
  uint32_t sessionCompleted = 0;
  uint32_t totalLearned = 0;
  uint32_t dueReviews = 0;
  bool showingAnswer = false;
  FlashcardScheduler scheduler;

  bool loadIndex();
  bool buildIndex(uint32_t sourceSize, uint32_t sourceFingerprint);
  bool loadCard(int index);
  bool readIndexHeader(HalFile& indexFile, uint32_t sourceSize, uint32_t sourceFingerprint);
  bool configureColumnsFromHeader(const std::vector<std::string>& fields);
  bool selectNextCard();
  bool isTimeValid() const;
  void gradeCurrent(FlashcardGrade grade);
  static std::vector<std::string> parseCsvLine(const std::string& line);

 public:
  explicit FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckPath)
      : Activity("FlashcardReview", renderer, mappedInput), deckPath(std::move(deckPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
