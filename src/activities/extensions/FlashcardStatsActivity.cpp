#include "FlashcardStatsActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
namespace fui = freeink::ui;

constexpr fui::ActionId ACTION_FUI_ROW = 1;
constexpr char FLASHCARDS_DIR[] = "/flashcards";

struct CsvColumns {
  int word = -1;
  int definition = -1;
};

const char* viewTitle(const FlashcardStatsActivity::View view) {
  switch (view) {
    case FlashcardStatsActivity::View::Decks: return tr(STR_FLASHCARD_DECKS);
    case FlashcardStatsActivity::View::DeckDetail: return tr(STR_FLASHCARD_DECK_DETAIL);
    case FlashcardStatsActivity::View::Calendar: return tr(STR_FLASHCARD_CALENDAR);
    case FlashcardStatsActivity::View::DayDetail: return tr(STR_FLASHCARD_DAY_DETAIL);
    case FlashcardStatsActivity::View::Overview:
    default: return tr(STR_FLASHCARD_STATS_TITLE);
  }
}

std::string normalizedColumnName(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

  std::string normalized;
  normalized.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))));
  }
  return normalized;
}

std::vector<std::string> parseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool inQuotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
        field += '"';
        ++i;
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

CsvColumns columnsFromHeader(const std::vector<std::string>& fields) {
  CsvColumns columns;
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto name = normalizedColumnName(fields[i]);
    if (name == "word" || name == "term" || name == "front") columns.word = static_cast<int>(i);
    if (name == "definition" || name == "meaning" || name == "translation" || name == "back") {
      columns.definition = static_cast<int>(i);
    }
  }
  return columns;
}

bool validCardLine(const std::vector<std::string>& fields, const CsvColumns& columns) {
  return columns.word >= 0 && columns.definition >= 0 && fields.size() > static_cast<size_t>(columns.word) &&
         fields.size() > static_cast<size_t>(columns.definition);
}

uint32_t countCards(const std::string& deckPath) {
  HalFile source;
  if (!Storage.openFileForRead("FC", deckPath, source)) return 0;

  uint32_t count = 0;
  bool firstLine = true;
  CsvColumns columns;
  std::string line;
  line.reserve(128);
  const auto processLine = [&]() {
    const auto fields = parseCsvLine(line);
    if (firstLine) {
      columns = columnsFromHeader(fields);
      firstLine = false;
    } else if (validCardLine(fields, columns)) {
      ++count;
    }
    line.clear();
  };

  while (source.available() > 0) {
    const int ch = source.read();
    if (ch < 0) break;
    if (ch == '\n') processLine();
    else line += static_cast<char>(ch);
  }
  if (!line.empty()) processLine();
  source.close();
  return count;
}

uint32_t ordinalForDate(const std::string& date) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (date.size() != 10 || std::sscanf(date.c_str(), "%d-%u-%u", &year, &month, &day) != 3) return 0;
  return TimeUtils::getDayOrdinalForDate(year, month, day);
}

void drawMetric(const GfxRenderer& renderer, const int x, const int y, const int width, const char* label,
                const std::string& value) {
  renderer.drawText(UI_10_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
  renderer.drawText(UI_10_FONT_ID, x + width - valueWidth, y, value.c_str());
}

void drawProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                     const uint32_t value, const uint32_t max) {
  renderer.drawRect(x, y, width, height);
  const int fill = max == 0 ? 0 : static_cast<int>(std::min<uint64_t>(width - 2, (static_cast<uint64_t>(value) * (width - 2)) / max));
  if (fill > 0) renderer.fillRect(x + 1, y + 1, fill, height - 2);
}

void moveCalendarMonth(uint32_t& selectedOrdinal, const int delta) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 1;
  if (!TimeUtils::getDateFromDayOrdinal(selectedOrdinal, year, month, day)) return;
  int nextMonth = static_cast<int>(month) + delta;
  while (nextMonth < 1) { nextMonth += 12; --year; }
  while (nextMonth > 12) { nextMonth -= 12; ++year; }
  day = std::min(day, TimeUtils::getDaysInMonth(year, static_cast<unsigned>(nextMonth)));
  selectedOrdinal = TimeUtils::getDayOrdinalForDate(year, static_cast<unsigned>(nextMonth), day);
}

void moveCalendarDay(uint32_t& selectedOrdinal, const int delta) {
  if (delta < 0 && selectedOrdinal > 0) --selectedOrdinal;
  if (delta > 0) ++selectedOrdinal;
}
}  // namespace

void FlashcardStatsActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_FUI_ROW, &FlashcardStatsActivity::onFuiRow, this);
  app.setScreen(&FlashcardStatsActivity::fuiScreen, this);
  fuiNav_.reset();
  view = View::Overview;
  selectedIndex = 0;
  refreshData();
  selectedDayOrdinal = referenceDayOrdinal();
  requestUpdate(true);
}

void FlashcardStatsActivity::onExit() {
  closeRouting();
  fuiRows_.clear();
  fuiLabels_.clear();
  fuiValues_.clear();
  decks.clear();
  dailyEntries.clear();
  Activity::onExit();
}

void FlashcardStatsActivity::refreshData() {
  decks.clear();
  totals = {};
  dailyEntries = FLASHCARD_STATS.getRecentDailyEntries();

  auto root = Storage.open(FLASHCARDS_DIR);
  if (root && root.isDirectory()) {
    char name[160];
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
      file.getName(name, sizeof(name));
      const std::string filename(name);
      const bool isDeck = !file.isDirectory() && FsHelpers::checkFileExtension(filename, ".csv");
      file.close();
      if (!isDeck) continue;

      DeckSummary summary;
      summary.name = filename;
      const std::string path = std::string(FLASHCARDS_DIR) + "/" + filename;
      summary.cardCount = countCards(path);

      FlashcardScheduler scheduler;
      scheduler.load(path);
      summary.learnedCount = scheduler.learnedCount();
      summary.totalReviews = scheduler.getTotalReviews();
      summary.currentStreak = scheduler.getCurrentStreak();
      summary.maxStreak = scheduler.getMaxStreak();
      summary.completedToday = scheduler.getCompletedToday();
      const time_t now = time(nullptr);
      for (const auto& record : scheduler.getRecords()) {
        const auto state = static_cast<FlashcardSrsState>(record.state);
        if (state == FlashcardSrsState::LEARNING) ++summary.learningCount;
        else if (state == FlashcardSrsState::RELEARNING) ++summary.relearningCount;
        else ++summary.reviewCount;
        summary.lapses += record.lapses;
        if (scheduler.hasValidTime() && record.dueAt <= now) ++summary.dueCount;
      }
      if (summary.cardCount < summary.learnedCount) summary.cardCount = summary.learnedCount;
      totals.cardCount += summary.cardCount;
      totals.learnedCount += summary.learnedCount;
      totals.dueCount += summary.dueCount;
      totals.learningCount += summary.learningCount;
      totals.relearningCount += summary.relearningCount;
      totals.reviewCount += summary.reviewCount;
      totals.lapses += summary.lapses;
      totals.totalReviews += summary.totalReviews;
      totals.completedToday += summary.completedToday;
      totals.fallbackCurrentStreak = std::max(totals.fallbackCurrentStreak, summary.currentStreak);
      totals.fallbackMaxStreak = std::max(totals.fallbackMaxStreak, summary.maxStreak);
      decks.push_back(std::move(summary));
    }
    root.close();
  }
  std::sort(decks.begin(), decks.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, static_cast<int>(decks.size()) - 1));
}

uint32_t FlashcardStatsActivity::referenceDayOrdinal() const {
  const uint32_t current = TimeUtils::getLocalDayOrdinal(TimeUtils::getCurrentValidTimestamp());
  if (current) return current;
  return dailyEntries.empty() ? 0 : ordinalForDate(dailyEntries.front().date);
}

const FlashcardDailyStats* FlashcardStatsActivity::statsForDay(const uint32_t ordinal) const {
  if (!ordinal) return nullptr;
  for (const auto& entry : dailyEntries) {
    if (ordinalForDate(entry.date) == ordinal) return &entry;
  }
  return nullptr;
}

uint32_t FlashcardStatsActivity::completedForDay(const uint32_t ordinal) const {
  const auto* entry = statsForDay(ordinal);
  return entry ? entry->completed : 0;
}

FlashcardDailyStats FlashcardStatsActivity::recentStats(const uint32_t days) const {
  FlashcardDailyStats result;
  const uint32_t reference = referenceDayOrdinal();
  if (!reference || !days) return result;
  const uint32_t first = reference >= days - 1 ? reference - days + 1 : 0;
  for (const auto& entry : dailyEntries) {
    const uint32_t ordinal = ordinalForDate(entry.date);
    if (ordinal < first || ordinal > reference) continue;
    result.completed += entry.completed;
    result.newCards += entry.newCards;
    result.learningReviews += entry.learningReviews;
    result.reviewReviews += entry.reviewReviews;
    result.again += entry.again;
    result.hard += entry.hard;
    result.good += entry.good;
  }
  return result;
}

uint32_t FlashcardStatsActivity::recentCompleted(const uint32_t days) const {
  return recentStats(days).completed;
}

uint32_t FlashcardStatsActivity::currentStreak() const {
  uint32_t day = referenceDayOrdinal();
  if (!day) return totals.fallbackCurrentStreak;
  if (!completedForDay(day) && day > 0) --day;
  if (!completedForDay(day)) return 0;
  uint32_t streak = 0;
  while (completedForDay(day)) {
    ++streak;
    if (!day) break;
    --day;
  }
  return streak;
}

uint32_t FlashcardStatsActivity::maxStreak() const {
  if (dailyEntries.empty()) return totals.fallbackMaxStreak;
  std::vector<uint32_t> ordinals;
  ordinals.reserve(dailyEntries.size());
  for (const auto& entry : dailyEntries) {
    if (entry.completed) ordinals.push_back(ordinalForDate(entry.date));
  }
  std::sort(ordinals.begin(), ordinals.end());
  uint32_t best = 0;
  uint32_t run = 0;
  uint32_t previous = 0;
  for (const auto ordinal : ordinals) {
    if (run && ordinal == previous + 1) ++run;
    else run = 1;
    best = std::max(best, run);
    previous = ordinal;
  }
  return std::max(best, static_cast<uint32_t>(totals.fallbackMaxStreak));
}

void FlashcardStatsActivity::fuiScreen(UiScreen& screen, void* user) {
  static_cast<FlashcardStatsActivity*>(user)->buildFuiScreen(screen);
}

void FlashcardStatsActivity::onFuiRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FlashcardStatsActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int>(self->decks.size())) return;
  self->selectedIndex = event.value;
  self->view = View::DeckDetail;
  self->fuiNav_.reset(self->selectedIndex);
  self->closeRouting();
  self->app.clearTapFlash();
  self->requestUpdate();
}

void FlashcardStatsActivity::buildFuiScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                       static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (decks.empty()) {
    screen.centeredText(tr(STR_NO_FLASHCARD_DECKS), screen.theme().bodyText);
    return;
  }

  fuiLabels_.clear();
  fuiValues_.clear();
  fuiRows_.clear();
  fuiLabels_.reserve(decks.size());
  fuiValues_.reserve(decks.size());
  fuiRows_.reserve(decks.size());
  for (const auto& deck : decks) {
    fuiLabels_.push_back(deck.name);
    fuiValues_.push_back(std::to_string(deck.learnedCount) + "/" + std::to_string(deck.cardCount) + "  " +
                         tr(STR_FLASHCARD_DUE) + ":" + std::to_string(deck.dueCount));
  }
  for (size_t i = 0; i < decks.size(); ++i) {
    fui::ListItem item;
    item.label = fuiLabels_[i].c_str();
    item.value = fuiValues_[i].c_str();
    item.icon = {};
    item.actionValue = static_cast<int16_t>(i);
    fuiRows_.push_back(item);
  }

  fui::ListProps props;
  props.items = fuiRows_.data();
  props.count = static_cast<uint16_t>(fuiRows_.size());
  props.action = ACTION_FUI_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.valueText = screen.theme().smallText;
  props.valueInset = 8;
  const int rowHeight = mappedInput.hasTouch()
                            ? screen.theme().rowHeight
                            : UITheme::getInstance().getMetrics().listWithSubtitleRowHeight;
  props.rowHeight = static_cast<int16_t>(rowHeight);
  selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(decks.size()) - 1);
  fuiNav_.selected = selectedIndex;
  fuiNav_.syncToProps(screen.body(), props.rowHeight, screen.theme().listRowGap, static_cast<int>(decks.size()), props);
  screen.list(props);
}

bool FlashcardStatsActivity::routeFuiTouch() {
  if (view != View::Decks) return false;
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed) {
    if (app.invalidated()) requestUpdate();
    return static_cast<bool>(route.event);
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? fuiNav_.pageRows() : -fuiNav_.pageRows();
    if (fuiNav_.scrollBy(delta, static_cast<int>(decks.size()))) requestUpdate();
    return true;
  }
  return false;
}

void FlashcardStatsActivity::moveVertical(const int delta) {
  if (view != View::Decks || decks.empty()) return;
  selectedIndex = delta > 0 ? ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(decks.size()))
                            : ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(decks.size()));
  requestUpdate();
}

void FlashcardStatsActivity::loop() {
  if (routeFuiTouch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::DeckDetail) { view = View::Decks; requestUpdate(); return; }
    if (view == View::DayDetail) { view = View::Calendar; requestUpdate(); return; }
    if (view != View::Overview) { view = View::Overview; requestUpdate(); return; }
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (view == View::Overview) view = View::Decks;
    else if (view == View::Decks && !decks.empty()) view = View::DeckDetail;
    else if (view == View::Calendar && selectedDayOrdinal) view = View::DayDetail;
    requestUpdate();
    return;
  }

  if (view == View::Calendar) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) { moveCalendarDay(selectedDayOrdinal, -1); requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) { moveCalendarDay(selectedDayOrdinal, 1); requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) { moveCalendarMonth(selectedDayOrdinal, -1); requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) { moveCalendarMonth(selectedDayOrdinal, 1); requestUpdate(); return; }
  } else if (view == View::Overview) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) { view = View::Decks; requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) { view = View::Calendar; requestUpdate(); return; }
  }

  buttonNavigator.onNextRelease([this] { moveVertical(1); });
  buttonNavigator.onPreviousRelease([this] { moveVertical(-1); });
}

void FlashcardStatsActivity::drawFooter(const char* confirmLabel) const {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel ? confirmLabel : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardStatsActivity::renderOverview() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - x * 2;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (decks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_FLASHCARD_DECKS));
    drawFooter();
    return;
  }

  drawMetric(renderer, x, top, width, tr(STR_FLASHCARD_TOTAL_CARDS), std::to_string(totals.cardCount));
  drawMetric(renderer, x, top + 28, width, tr(STR_FLASHCARD_LEARNED),
             std::to_string(totals.learnedCount) + " / " + std::to_string(totals.cardCount));
  drawProgressBar(renderer, x, top + 52, width, 13, totals.learnedCount, totals.cardCount);
  drawMetric(renderer, x, top + 82, width / 2 - 8, tr(STR_FLASHCARD_DUE), std::to_string(totals.dueCount));
  drawMetric(renderer, x + width / 2, top + 82, width / 2, tr(STR_LAST_7_DAYS),
             std::to_string(recentCompleted(7)));
  drawMetric(renderer, x, top + 110, width / 2 - 8, tr(STR_FLASHCARD_TODAY), std::to_string(totals.completedToday));
  drawMetric(renderer, x + width / 2, top + 110, width / 2, tr(STR_FLASHCARD_TOTAL_REVIEWS),
             std::to_string(totals.totalReviews));
  drawMetric(renderer, x, top + 138, width / 2 - 8, tr(STR_FLASHCARD_STREAK), std::to_string(currentStreak()));
  drawMetric(renderer, x + width / 2, top + 138, width / 2, tr(STR_FLASHCARD_BEST_STREAK), std::to_string(maxStreak()));

  renderer.drawText(UI_10_FONT_ID, x, top + 174, tr(STR_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  const int chartY = top + 194;
  const int barWidth = std::max(8, (width - 12) / 7);
  const uint32_t reference = referenceDayOrdinal();
  uint32_t maxValue = 1;
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = reference >= static_cast<uint32_t>(6 - i) ? reference - 6 + i : static_cast<uint32_t>(i);
    maxValue = std::max(maxValue, completedForDay(ordinal));
  }
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = reference >= static_cast<uint32_t>(6 - i) ? reference - 6 + i : static_cast<uint32_t>(i);
    const int height = static_cast<int>(completedForDay(ordinal) * 75ULL / maxValue);
    renderer.fillRect(x + i * barWidth, chartY + 75 - height, barWidth - 3, std::max(2, height));
  }
  drawMetric(renderer, x, chartY + 91, width, tr(STR_RECENT_30_DAYS), std::to_string(recentCompleted(30)));
  renderer.drawText(UI_10_FONT_ID, x, chartY + 126, tr(STR_FLASHCARD_STATE), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, chartY + 151,
                    (std::string(tr(STR_FLASHCARD_LEARNING)) + ":" + std::to_string(totals.learningCount) + "  " +
                     tr(STR_FLASHCARD_RELEARNING) + ":" + std::to_string(totals.relearningCount) + "  " +
                     tr(STR_FLASHCARD_REVIEW) + ":" + std::to_string(totals.reviewCount)).c_str());
  const auto recent = recentStats(30);
  renderer.drawText(UI_10_FONT_ID, x, chartY + 179,
                    (std::string(tr(STR_RECENT_30_DAYS)) + "  " + tr(STR_FLASHCARD_AGAIN) + ":" + std::to_string(recent.again) + "  " +
                     tr(STR_FLASHCARD_HARD) + ":" + std::to_string(recent.hard) + "  " +
                     tr(STR_FLASHCARD_GOOD) + ":" + std::to_string(recent.good)).c_str());
  drawFooter(tr(STR_FLASHCARD_DECKS));
}

void FlashcardStatsActivity::renderDeckDetail() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - x * 2;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(decks.size())) {
    renderer.drawText(UI_10_FONT_ID, x, top, tr(STR_NO_FLASHCARD_DECKS));
    drawFooter();
    return;
  }
  const auto& deck = decks[static_cast<size_t>(selectedIndex)];
  renderer.drawText(UI_12_FONT_ID, x, top, renderer.truncatedText(UI_12_FONT_ID, deck.name.c_str(), width).c_str(), true,
                    EpdFontFamily::BOLD);
  drawMetric(renderer, x, top + 36, width, tr(STR_FLASHCARD_TOTAL_CARDS), std::to_string(deck.cardCount));
  drawMetric(renderer, x, top + 64, width, tr(STR_FLASHCARD_LEARNED), std::to_string(deck.learnedCount));
  drawProgressBar(renderer, x, top + 88, width, 13, deck.learnedCount, deck.cardCount);
  drawMetric(renderer, x, top + 120, width, tr(STR_FLASHCARD_DUE), std::to_string(deck.dueCount));
  drawMetric(renderer, x, top + 148, width, tr(STR_FLASHCARD_TOTAL_REVIEWS), std::to_string(deck.totalReviews));
  drawMetric(renderer, x, top + 176, width, tr(STR_FLASHCARD_LEARNING), std::to_string(deck.learningCount));
  drawMetric(renderer, x, top + 204, width, tr(STR_FLASHCARD_RELEARNING), std::to_string(deck.relearningCount));
  drawMetric(renderer, x, top + 232, width, tr(STR_FLASHCARD_REVIEW), std::to_string(deck.reviewCount));
  drawMetric(renderer, x, top + 260, width, tr(STR_FLASHCARD_LAPSES), std::to_string(deck.lapses));
  drawMetric(renderer, x, top + 288, width, tr(STR_FLASHCARD_STREAK), std::to_string(deck.currentStreak));
  drawMetric(renderer, x, top + 316, width, tr(STR_FLASHCARD_BEST_STREAK), std::to_string(deck.maxStreak));
  drawFooter();
}

void FlashcardStatsActivity::renderCalendar() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int cell = std::min(48, static_cast<int>((renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / 7));
  const int left = (renderer.getScreenWidth() - cell * 7) / 2;
  if (!selectedDayOrdinal) selectedDayOrdinal = referenceDayOrdinal();
  if (!selectedDayOrdinal) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_FLASHCARD_STATS));
    drawFooter();
    return;
  }
  int year = 0;
  unsigned month = 0;
  unsigned day = 1;
  TimeUtils::getDateFromDayOrdinal(selectedDayOrdinal, year, month, day);
  const uint32_t firstDay = TimeUtils::getDayOrdinalForDate(year, month, 1);
  const unsigned daysInMonth = TimeUtils::getDaysInMonth(year, month);
  const int firstWeekday = static_cast<int>((firstDay + 4) % 7);
  char monthTitle[16];
  std::snprintf(monthTitle, sizeof(monthTitle), "%04d-%02u", year, month);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top, monthTitle, true, EpdFontFamily::BOLD);
  static constexpr const char* WEEKDAYS[] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int weekday = 0; weekday < 7; ++weekday) {
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, WEEKDAYS[weekday]);
    renderer.drawText(SMALL_FONT_ID, left + weekday * cell + (cell - textWidth) / 2, top + 20, WEEKDAYS[weekday], true);
  }
  uint32_t maxValue = 1;
  for (unsigned number = 1; number <= daysInMonth; ++number) {
    maxValue = std::max(maxValue, completedForDay(firstDay + number - 1));
  }
  for (int i = 0; i < 42; ++i) {
    const int number = i - firstWeekday + 1;
    if (number < 1 || number > static_cast<int>(daysInMonth)) continue;
    const uint32_t ordinal = firstDay + static_cast<uint32_t>(number - 1);
    const int x = left + (i % 7) * cell;
    const int y = top + 30 + (i / 7) * 42;
    const uint32_t value = completedForDay(ordinal);
    const bool selected = ordinal == selectedDayOrdinal;
    if (selected) renderer.fillRect(x + 1, y + 1, cell - 3, 31, true);
    else if (value > 0) {
      const int level = static_cast<int>(value * 3ULL / maxValue);
      if (level >= 2) renderer.fillRectDither(x + 1, y + 1, cell - 3, 31, Color::DarkGray);
      else renderer.fillRectDither(x + 1, y + 1, cell - 3, 31, Color::LightGray);
    }
    renderer.drawRect(x + 1, y + 1, cell - 3, 31, !selected);
    char dayText[8];
    std::snprintf(dayText, sizeof(dayText), "%02d", number);
    renderer.drawCenteredText(SMALL_FONT_ID, y + 10, dayText, !selected);
  }
  const auto* selectedStats = statsForDay(selectedDayOrdinal);
  const uint32_t infoY = top + 30 + 6 * 42 + 8;
  drawMetric(renderer, metrics.contentSidePadding, infoY, renderer.getScreenWidth() - 2 * metrics.contentSidePadding,
             tr(STR_SELECTED_DAY), TimeUtils::formatDateParts(year, month, day) + "  " +
                                         std::to_string(selectedStats ? selectedStats->completed : 0) + " " +
                                         tr(STR_FLASHCARD_COMPLETED));
  drawFooter(tr(STR_FLASHCARD_DAY_DETAIL));
}

void FlashcardStatsActivity::renderDayDetail() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - 2 * x;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int year = 0;
  unsigned month = 0;
  unsigned day = 1;
  TimeUtils::getDateFromDayOrdinal(selectedDayOrdinal, year, month, day);
  const auto* stats = statsForDay(selectedDayOrdinal);
  const FlashcardDailyStats empty;
  if (!stats) stats = &empty;
  renderer.drawText(UI_12_FONT_ID, x, top, TimeUtils::formatDateParts(year, month, day).c_str(), true,
                    EpdFontFamily::BOLD);
  drawMetric(renderer, x, top + 38, width, tr(STR_FLASHCARD_COMPLETED), std::to_string(stats->completed));
  drawMetric(renderer, x, top + 68, width, tr(STR_FLASHCARD_NEW_CARDS), std::to_string(stats->newCards));
  drawMetric(renderer, x, top + 98, width, tr(STR_FLASHCARD_LEARNING), std::to_string(stats->learningReviews));
  drawMetric(renderer, x, top + 128, width, tr(STR_FLASHCARD_REVIEW), std::to_string(stats->reviewReviews));
  drawMetric(renderer, x, top + 168, width, tr(STR_FLASHCARD_AGAIN), std::to_string(stats->again));
  drawMetric(renderer, x, top + 198, width, tr(STR_FLASHCARD_HARD), std::to_string(stats->hard));
  drawMetric(renderer, x, top + 228, width, tr(STR_FLASHCARD_GOOD), std::to_string(stats->good));
  drawFooter();
}

void FlashcardStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, UITheme::getInstance().getMetrics().topPadding, renderer.getScreenWidth(),
                                UITheme::getInstance().getMetrics().headerHeight}, viewTitle(view));
  if (view == View::Decks) {
    renderUi();
    drawFooter(tr(STR_FLASHCARD_DECK_DETAIL));
    renderer.displayBuffer();
    return;
  }
  switch (view) {
    case View::DeckDetail: renderDeckDetail(); break;
    case View::Calendar: renderCalendar(); break;
    case View::DayDetail: renderDayDetail(); break;
    case View::Decks: break;
    case View::Overview:
    default: renderOverview(); break;
  }
  renderer.displayBuffer();
}
