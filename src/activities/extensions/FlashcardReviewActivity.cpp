#include "FlashcardReviewActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

std::vector<std::string> FlashcardReviewActivity::parseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool inQuotes = false;

  for (size_t i = 0; i < line.size(); i++) {
    const char c = line[i];
    if (c == '"') {
      if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
        field += '"';
        i++;
      } else {
        inQuotes = !inQuotes;
      }
    } else if (c == ',' && !inQuotes) {
      fields.push_back(field);
      field.clear();
    } else if (c != '\r') {
      field += c;
    }
  }
  fields.push_back(field);
  return fields;
}

bool FlashcardReviewActivity::loadCards() {
  cards.clear();

  HalFile file;
  if (!Storage.openFileForRead("FC", deckPath, file)) {
    return false;
  }

  std::string line;
  bool firstLine = true;
  while (file.available() > 0) {
    const int ch = file.read();
    if (ch < 0) break;
    if (ch == '\n') {
      auto fields = parseCsvLine(line);
      line.clear();
      if (fields.size() >= 3) {
        if (firstLine && fields[0] == "word") {
          firstLine = false;
          continue;
        }
        cards.push_back({fields[0], fields[1], fields[2]});
      }
      firstLine = false;
    } else {
      line += static_cast<char>(ch);
    }
  }
  if (!line.empty()) {
    auto fields = parseCsvLine(line);
    if (fields.size() >= 3 && !(firstLine && fields[0] == "word")) {
      cards.push_back({fields[0], fields[1], fields[2]});
    }
  }
  file.close();
  return true;
}

void FlashcardReviewActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  showingAnswer = false;
  loadCards();
  requestUpdate();
}

void FlashcardReviewActivity::onExit() {
  Activity::onExit();
  cards.clear();
}

void FlashcardReviewActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showingAnswer = !showingAnswer;
    requestUpdate();
    return;
  }

  const int listSize = static_cast<int>(cards.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, listSize);
    showingAnswer = false;
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, listSize);
    showingAnswer = false;
    requestUpdate();
  });
}

void FlashcardReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (cards.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FLASHCARDS));
  } else {
    const auto& card = cards[selectedIndex];
    char counter[32];
    snprintf(counter, sizeof(counter), "%d/%d", selectedIndex + 1, static_cast<int>(cards.size()));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, contentTop, counter);

    std::string prewarm = card.word + "\n" + card.phonetic + "\n" + card.definition;
    const int cardFontId = DynamicFont::fontForCjkText(renderer, prewarm.c_str(), UI_12_FONT_ID);
    DynamicFont::prewarmIfSdFont(renderer, cardFontId, prewarm);
    const auto cardStyle = renderer.isSdCardFont(cardFontId) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;

    renderer.drawCenteredText(cardFontId, contentTop + 55, card.word.c_str(), true, cardStyle);
    if (!card.phonetic.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 95, card.phonetic.c_str());
    }

    if (showingAnswer) {
      const int boxY = contentTop + 135;
      renderer.drawLine(metrics.contentSidePadding, boxY - 10, pageWidth - metrics.contentSidePadding, boxY - 10);
      renderer.drawText(cardFontId, metrics.contentSidePadding, boxY, card.definition.c_str(), true);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, tr(STR_FLASHCARD_SHOW_ANSWER));
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
