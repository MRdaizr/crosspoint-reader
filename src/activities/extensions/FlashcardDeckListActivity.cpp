#include "FlashcardDeckListActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "FlashcardReviewActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* FLASHCARDS_DIR = "/flashcards";
}

void FlashcardDeckListActivity::loadDecks() {
  decks.clear();
  Storage.mkdir(FLASHCARDS_DIR);

  auto root = Storage.open(FLASHCARDS_DIR);
  if (!root || !root.isDirectory() || !fileNameBuffer) {
    return;
  }

  root.rewindDirectory();
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    std::string_view filename{fileNameBuffer.get()};
    if (!file.isDirectory() && FsHelpers::checkFileExtension(filename, ".csv")) {
      decks.emplace_back(filename);
    }
  }
  root.close();
  std::sort(decks.begin(), decks.end());
}

void FlashcardDeckListActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  selectedIndex = 0;
  loadDecks();
  requestUpdate();
}

void FlashcardDeckListActivity::onExit() {
  Activity::onExit();
  decks.clear();
  fileNameBuffer.reset();
}

void FlashcardDeckListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (decks.empty()) return;
    std::string path = std::string(FLASHCARDS_DIR) + "/" + decks[selectedIndex];
    startActivityForResult(std::make_unique<FlashcardReviewActivity>(renderer, mappedInput, std::move(path)), nullptr);
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int listSize = static_cast<int>(decks.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });
}

void FlashcardDeckListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (decks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FLASHCARD_DECKS));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 45, "/flashcards/*.csv");
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(decks.size()), selectedIndex,
                 [this](int index) { return decks[index]; }, nullptr, [](int) { return UIIcon::File; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
