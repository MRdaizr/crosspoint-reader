#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FlashcardDeckListActivity final : public Activity {
  static constexpr size_t NAME_BUFFER_SIZE = 256;
  ButtonNavigator buttonNavigator;
  std::vector<std::string> decks;
  std::unique_ptr<char[]> fileNameBuffer;
  int selectedIndex = 0;

  void loadDecks();

 public:
  explicit FlashcardDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FlashcardDecks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
