#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class FlashcardDeckListActivity final : public UiListActivity {
  static constexpr size_t NAME_BUFFER_SIZE = 256;
  std::vector<std::string> decks;
  std::vector<freeink::ui::ListItem> rowItems;
  std::unique_ptr<char[]> fileNameBuffer;
  void loadDecks();
  void rebuildRowItems();
  int listCount() const override { return static_cast<int>(decks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

 public:
  explicit FlashcardDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("FlashcardDecks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
};
