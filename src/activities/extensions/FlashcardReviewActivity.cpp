#include "FlashcardReviewActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
constexpr uint32_t FLASHCARD_INDEX_MAGIC = 0x31494346;  // "FCI1"
constexpr uint32_t FLASHCARD_INDEX_VERSION = 3;

struct FlashcardIndexHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sourceSize;
  uint32_t sourceFingerprint;
  uint32_t cardCount;
  uint8_t wordColumn;
  uint8_t phoneticColumn;
  uint8_t definitionColumn;
};

struct FlashcardIndexEntry {
  uint32_t offset;
  uint64_t cardId;
};

std::string normalizedColumnName(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) begin++;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;

  std::string normalized;
  normalized.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))));
  }
  return normalized;
}

uint32_t updateFingerprint(uint32_t hash, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t fileFingerprint(HalFile& file, const uint32_t fileSize) {
  uint8_t buffer[256];
  uint32_t hash = 2166136261u;
  if (!file.seek(0)) return 0;
  uint32_t remaining = fileSize;
  while (remaining > 0) {
    const size_t bytes = std::min<size_t>(remaining, sizeof(buffer));
    if (file.read(buffer, bytes) != static_cast<int>(bytes)) return 0;
    hash = updateFingerprint(hash, buffer, bytes);
    remaining -= bytes;
  }
  return hash;
}
}  // namespace

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
  if (!fields.empty() && fields[0].size() >= 3 && static_cast<uint8_t>(fields[0][0]) == 0xEF &&
      static_cast<uint8_t>(fields[0][1]) == 0xBB && static_cast<uint8_t>(fields[0][2]) == 0xBF) {
    fields[0].erase(0, 3);
  }
  return fields;
}

bool FlashcardReviewActivity::readIndexHeader(HalFile& indexFile, const uint32_t sourceSize,
                                               const uint32_t sourceFingerprint) {
  FlashcardIndexHeader header{};
  if (indexFile.read(&header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  return header.magic == FLASHCARD_INDEX_MAGIC && header.version == FLASHCARD_INDEX_VERSION &&
         header.sourceSize == sourceSize && header.sourceFingerprint == sourceFingerprint;
}

bool FlashcardReviewActivity::configureColumnsFromHeader(const std::vector<std::string>& fields) {
  int word = -1;
  int phonetic = -1;
  int definition = -1;
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto name = normalizedColumnName(fields[i]);
    if (name == "word" || name == "term" || name == "front") word = static_cast<int>(i);
    if (name == "phonetic" || name == "pronunciation" || name == "ipa") phonetic = static_cast<int>(i);
    if (name == "definition" || name == "meaning" || name == "translation" || name == "back") {
      definition = static_cast<int>(i);
    }
  }
  if (word < 0) return false;

  wordColumn = static_cast<uint8_t>(word);
  phoneticColumn = phonetic >= 0 ? static_cast<uint8_t>(phonetic) : UINT8_MAX;
  definitionColumn = definition >= 0 ? static_cast<uint8_t>(definition) : UINT8_MAX;
  return true;
}

bool FlashcardReviewActivity::buildIndex(const uint32_t sourceSize, const uint32_t sourceFingerprint) {
  const std::string tempPath = indexPath + ".tmp";
  Storage.remove(tempPath.c_str());

  HalFile source;
  HalFile indexFile;
  if (!Storage.openFileForRead("FC", deckPath, source) ||
      !Storage.openFileForWrite("FC", tempPath, indexFile)) {
    source.close();
    indexFile.close();
    Storage.remove(tempPath.c_str());
    return false;
  }

  const FlashcardIndexHeader emptyHeader{FLASHCARD_INDEX_MAGIC, FLASHCARD_INDEX_VERSION, sourceSize,
                                         sourceFingerprint, 0, wordColumn, phoneticColumn, definitionColumn};
  if (indexFile.write(&emptyHeader, sizeof(emptyHeader)) != sizeof(emptyHeader)) {
    source.close();
    indexFile.close();
    Storage.remove(tempPath.c_str());
    return false;
  }

  uint32_t count = 0;
  uint32_t lineStart = 0;
  bool firstLine = true;
  std::string line;
  line.reserve(128);

  const auto indexLine = [&](const uint32_t offset, const std::string& csvLine, const bool isFirstLine) {
    const auto fields = parseCsvLine(csvLine);
    if (isFirstLine && configureColumnsFromHeader(fields)) {
      return true;
    }
    if (wordColumn == UINT8_MAX || definitionColumn == UINT8_MAX) {
      return true;
    }
    const size_t requiredColumn = std::max(static_cast<size_t>(wordColumn), static_cast<size_t>(definitionColumn));
    if (fields.size() <= requiredColumn) return true;
    const uint64_t cardId = FlashcardScheduler::cardId(fields[wordColumn],
                                                        phoneticColumn != UINT8_MAX && fields.size() > phoneticColumn
                                                            ? fields[phoneticColumn] : "",
                                                        fields[definitionColumn]);
    const FlashcardIndexEntry entry{offset, cardId};
    if (indexFile.write(&entry, sizeof(entry)) != sizeof(entry)) {
      return false;
    }
    ++count;
    return true;
  };

  while (source.available() > 0) {
    const int ch = source.read();
    if (ch < 0) break;
    if (ch == '\n') {
      if (!indexLine(lineStart, line, firstLine)) {
        source.close();
        indexFile.close();
        Storage.remove(tempPath.c_str());
        return false;
      }
      line.clear();
      lineStart = static_cast<uint32_t>(source.position());
      firstLine = false;
    } else {
      line += static_cast<char>(ch);
    }
  }
  if (!line.empty() && !indexLine(lineStart, line, firstLine)) {
    source.close();
    indexFile.close();
    Storage.remove(tempPath.c_str());
    return false;
  }

  const FlashcardIndexHeader completeHeader{FLASHCARD_INDEX_MAGIC, FLASHCARD_INDEX_VERSION, sourceSize,
                                            sourceFingerprint, count, wordColumn, phoneticColumn, definitionColumn};
  if (!indexFile.seek(0) || indexFile.write(&completeHeader, sizeof(completeHeader)) != sizeof(completeHeader)) {
    source.close();
    indexFile.close();
    Storage.remove(tempPath.c_str());
    return false;
  }
  source.close();
  indexFile.close();

  Storage.remove(indexPath.c_str());
  if (!Storage.rename(tempPath.c_str(), indexPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  cardCount = count;
  return true;
}

bool FlashcardReviewActivity::loadIndex() {
  cardCount = 0;
  HalFile file;
  if (!Storage.openFileForRead("FC", deckPath, file)) {
    return false;
  }
  const uint32_t sourceSize = static_cast<uint32_t>(file.fileSize());
  const uint32_t sourceFingerprint = fileFingerprint(file, sourceSize);
  file.close();

  if (sourceFingerprint == 0) return false;

  indexPath = deckPath + ".idx";
  HalFile indexFile;
  bool valid = Storage.openFileForRead("FC", indexPath, indexFile) &&
               readIndexHeader(indexFile, sourceSize, sourceFingerprint);
  if (valid) {
    FlashcardIndexHeader header{};
    indexFile.seek(0);
    indexFile.read(&header, sizeof(header));
    const uint64_t expectedSize = sizeof(header) + static_cast<uint64_t>(header.cardCount) * sizeof(FlashcardIndexEntry);
    valid = indexFile.fileSize64() == expectedSize;
    if (valid) {
      cardCount = header.cardCount;
      wordColumn = header.wordColumn;
      phoneticColumn = header.phoneticColumn;
      definitionColumn = header.definitionColumn;
    }
  }
  indexFile.close();

  return valid || buildIndex(sourceSize, sourceFingerprint);
}

bool FlashcardReviewActivity::loadCard(const int index) {
  if (index < 0 || static_cast<uint32_t>(index) >= cardCount) return false;

  HalFile indexFile;
  HalFile source;
  if (!Storage.openFileForRead("FC", indexPath, indexFile) || !Storage.openFileForRead("FC", deckPath, source)) {
    indexFile.close();
    source.close();
    return false;
  }

  const size_t offsetPosition = sizeof(FlashcardIndexHeader) + static_cast<size_t>(index) * sizeof(FlashcardIndexEntry);
  FlashcardIndexEntry entry{};
  if (!indexFile.seek(offsetPosition) || indexFile.read(&entry, sizeof(entry)) != sizeof(entry) ||
      !source.seek(entry.offset)) {
    indexFile.close();
    source.close();
    return false;
  }

  std::string line;
  line.reserve(128);
  while (source.available() > 0) {
    const int ch = source.read();
    if (ch < 0 || ch == '\n') break;
    line += static_cast<char>(ch);
  }
  indexFile.close();
  source.close();

  const auto fields = parseCsvLine(line);
  if (wordColumn == UINT8_MAX || definitionColumn == UINT8_MAX || fields.size() <= wordColumn ||
      fields.size() <= definitionColumn) {
    return false;
  }
  currentCard.word = fields[wordColumn];
  currentCard.phonetic = phoneticColumn != UINT8_MAX && fields.size() > phoneticColumn ? fields[phoneticColumn] : "";
  currentCard.definition = fields[definitionColumn];
  currentCardId = entry.cardId;
  return true;
}

void FlashcardReviewActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  showingAnswer = false;
  loadIndex();
  scheduler.load(deckPath);
  selectNextCard();
  requestUpdate();
}

void FlashcardReviewActivity::onExit() {
  Activity::onExit();
  currentCard = {};
  cardCount = 0;
  wordColumn = 0;
  phoneticColumn = 1;
  definitionColumn = 2;
  indexPath.clear();
  currentCardId = 0;
}

bool FlashcardReviewActivity::isTimeValid() const { return scheduler.hasValidTime(); }

bool FlashcardReviewActivity::selectNextCard() {
  currentCard = {};
  currentCardId = 0;
  showingAnswer = false;
  if (cardCount == 0 || !isTimeValid()) return false;

  HalFile indexFile;
  if (!Storage.openFileForRead("FC", indexPath, indexFile)) return false;
  const time_t now = time(nullptr);
  int firstNew = -1;
  int selected = -1;
  uint32_t learningCount = 0;
  const bool allowReviews = scheduler.reviewCardsRemaining() > 0;
  for (uint32_t i = 0; i < cardCount; ++i) {
    FlashcardIndexEntry entry{};
    const size_t pos = sizeof(FlashcardIndexHeader) + static_cast<size_t>(i) * sizeof(entry);
    if (!indexFile.seek(pos) || indexFile.read(&entry, sizeof(entry)) != sizeof(entry)) break;
    const auto* record = scheduler.get(entry.cardId);
    if (!record) { if (firstNew < 0) firstNew = static_cast<int>(i); continue; }
    if (record->dueAt > now) continue;
    if (record->state != static_cast<uint8_t>(FlashcardSrsState::REVIEW)) {
      if (selected < 0) selected = static_cast<int>(i);
      ++learningCount;
    } else if (allowReviews && selected < 0) {
      selected = static_cast<int>(i);
    }
  }
  indexFile.close();
  if (selected < 0 && firstNew >= 0 && scheduler.newCardsRemaining() > 0) selected = firstNew;
  if (selected < 0) { totalLearned = scheduler.learnedCount(); dueReviews = scheduler.dueReviewCount(); return false; }
  selectedIndex = selected;
  if (!loadCard(selectedIndex)) return false;
  if (!scheduler.get(currentCardId) && !scheduler.introduce(currentCardId)) return false;
  totalLearned = scheduler.learnedCount();
  dueReviews = scheduler.dueReviewCount();
  return true;
}

void FlashcardReviewActivity::gradeCurrent(const FlashcardGrade grade) {
  if (currentCardId == 0 || !scheduler.grade(currentCardId, grade)) return;
  ++sessionCompleted;
  selectNextCard();
  requestUpdate();
}

void FlashcardReviewActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (showingAnswer) gradeCurrent(FlashcardGrade::GOOD);
    else { showingAnswer = true; requestUpdate(); }
    return;
  }
  if (showingAnswer && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    gradeCurrent(FlashcardGrade::AGAIN);
    return;
  }
  if (showingAnswer && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    gradeCurrent(FlashcardGrade::HARD);
    requestUpdate();
    return;
  }
}

void FlashcardReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (cardCount == 0) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FLASHCARDS));
  } else if (!isTimeValid()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_FLASHCARD_TIME_REQUIRED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_FLASHCARD_TIME_REQUIRED_DESC));
  } else if (currentCardId == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_FLASHCARD_DONE), true, EpdFontFamily::BOLD);
    char summary[64];
    snprintf(summary, sizeof(summary), "%s %lu  %s %lu", tr(STR_FLASHCARD_SESSION),
             static_cast<unsigned long>(sessionCompleted), tr(STR_FLASHCARD_LEARNED), static_cast<unsigned long>(totalLearned));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, summary);
  } else {
    const auto& card = currentCard;
    char counter[32];
    const unsigned long progress = cardCount == 0 ? 0 : static_cast<unsigned long>(totalLearned) * 100UL / cardCount;
    snprintf(counter, sizeof(counter), "%d/%d  %s:%lu (%lu%%)", selectedIndex + 1, static_cast<int>(cardCount),
             tr(STR_FLASHCARD_LEARNED), static_cast<unsigned long>(totalLearned), progress);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, contentTop, counter);

    std::string prewarm = card.word + "\n" + card.phonetic + "\n" + card.definition;
    const int cardFontId = DynamicFont::fontForCjkText(renderer, prewarm.c_str(), UI_12_FONT_ID);
    DynamicFont::prewarmIfSdFont(renderer, cardFontId, prewarm);
    int phoneticFontId = cardFontId;
    if (!card.phonetic.empty()) {
      sdFontSystem.ensureLoaded(renderer);
      const int sdFontId = sdFontSystem.currentFontId();
      if (renderer.isSdCardFont(sdFontId)) phoneticFontId = sdFontId;
      DynamicFont::prewarmIfSdFont(renderer, phoneticFontId, card.phonetic);
    }
    const auto cardStyle = renderer.isSdCardFont(cardFontId) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;

    renderer.drawCenteredText(cardFontId, contentTop + 55, card.word.c_str(), true, cardStyle);
    if (!card.phonetic.empty()) {
      renderer.drawCenteredText(phoneticFontId, contentTop + 95, card.phonetic.c_str());
    }

    if (showingAnswer) {
      const int boxY = contentTop + 135;
      renderer.drawLine(metrics.contentSidePadding, boxY - 10, pageWidth - metrics.contentSidePadding, boxY - 10);
      renderer.drawText(cardFontId, metrics.contentSidePadding, boxY, card.definition.c_str(), true);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, tr(STR_FLASHCARD_SHOW_ANSWER));
    }
  }

  const auto labels = showingAnswer ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_GOOD), tr(STR_FLASHCARD_AGAIN),
                                                             tr(STR_FLASHCARD_HARD))
                                    : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
