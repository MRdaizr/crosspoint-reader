#include "FlashcardDeckListActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "FlashcardReviewActivity.h"
#include "MappedInputManager.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FlashcardDeckUtils.h"

void FlashcardDeckListActivity::loadDecks() {
  decks.clear();
  Storage.mkdir(FlashcardDeckUtils::ROOTS[0]);
  if (!fileNameBuffer) {
    rowItems.clear();
    return;
  }

  for (size_t rootIndex = 0; rootIndex < FlashcardDeckUtils::ROOT_COUNT; ++rootIndex) {
    const char* rootPath = FlashcardDeckUtils::ROOTS[rootIndex];
    auto root = Storage.open(rootPath);
    if (!root || !root.isDirectory()) continue;

    root.rewindDirectory();
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
      file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      std::string_view filename{fileNameBuffer.get()};
      if (file.isDirectory() || !FsHelpers::checkFileExtension(filename, ".csv")) continue;

      DeckEntry deck;
      deck.name = filename;
      deck.path = std::string(rootPath) + "/" + deck.name;
      deck.label = deck.name;
      decks.push_back(std::move(deck));
    }
    root.close();
  }

  std::sort(decks.begin(), decks.end(), [](const DeckEntry& left, const DeckEntry& right) {
    if (left.name != right.name) return left.name < right.name;
    return left.path < right.path;
  });
  for (size_t i = 0; i < decks.size(); ++i) {
    const bool duplicateName = (i > 0 && decks[i - 1].name == decks[i].name) ||
                               (i + 1 < decks.size() && decks[i + 1].name == decks[i].name);
    if (duplicateName) {
      decks[i].label += decks[i].path.rfind("/.flashcards/", 0) == 0 ? " [/.flashcards]" : " [/flashcards]";
    }
  }
  rebuildRowItems();
}

void FlashcardDeckListActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(decks.size());
  for (size_t i = 0; i < decks.size(); ++i) {
    freeink::ui::ListItem item;
    item.label = decks[i].label.c_str();
    item.icon = GUI.showsFuiMenuIcon(FuiMenuIconSlot::FlashcardDeckRows) ? listIconFor(UIIcon::File)
                                                                          : freeink::ui::BitmapRef{};
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void FlashcardDeckListActivity::onEnter() {
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  loadDecks();
  UiListActivity::onEnter();
}

void FlashcardDeckListActivity::onExit() {
  UiListActivity::onExit();
  decks.clear();
  rowItems.clear();
  fileNameBuffer.reset();
}

void FlashcardDeckListActivity::activateIndex(const int index) {
    if (index < 0 || index >= static_cast<int>(decks.size())) return;
    std::string path = decks[index].path;
    startActivityForResult(std::make_unique<FlashcardReviewActivity>(renderer, mappedInput, std::move(path)), nullptr);
}

const char* FlashcardDeckListActivity::headerTitle() const { return tr(STR_FLASHCARDS); }

void FlashcardDeckListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                              static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (decks.empty()) {
    screen.centeredText(tr(STR_NO_FLASHCARD_DECKS), screen.theme().bodyText);
  } else {
    freeink::ui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = freeink::ui::InputTouch;
    props.labelText = screen.theme().bodyText;
    syncListViewport(screen, props);
    screen.list(props);
  }
}
