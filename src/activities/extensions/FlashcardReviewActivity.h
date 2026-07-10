#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HalFile;

struct FlashcardCard {
  std::string word;
  std::string phonetic;
  std::string definition;
};

class FlashcardReviewActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string deckPath;
  std::string indexPath;
  FlashcardCard currentCard;
  uint32_t cardCount = 0;
  int selectedIndex = 0;
  bool showingAnswer = false;

  bool loadIndex();
  bool buildIndex(uint32_t sourceSize, uint32_t sourceFingerprint);
  bool loadCard(int index);
  bool readIndexHeader(HalFile& indexFile, uint32_t sourceSize, uint32_t sourceFingerprint);
  static std::vector<std::string> parseCsvLine(const std::string& line);

 public:
  explicit FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckPath)
      : Activity("FlashcardReview", renderer, mappedInput), deckPath(std::move(deckPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
