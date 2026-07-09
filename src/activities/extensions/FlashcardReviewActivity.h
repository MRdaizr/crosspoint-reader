#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct FlashcardCard {
  std::string word;
  std::string phonetic;
  std::string definition;
};

class FlashcardReviewActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string deckPath;
  std::vector<FlashcardCard> cards;
  int selectedIndex = 0;
  bool showingAnswer = false;

  bool loadCards();
  static std::vector<std::string> parseCsvLine(const std::string& line);

 public:
  explicit FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckPath)
      : Activity("FlashcardReview", renderer, mappedInput), deckPath(std::move(deckPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
