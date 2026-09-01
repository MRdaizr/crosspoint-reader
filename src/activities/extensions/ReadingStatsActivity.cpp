#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

namespace {
namespace fui = freeink::ui;

constexpr fui::ActionId ACTION_FUI_ROW = 1;

const char* viewTitle(const ReadingStatsActivity::View view) {
  switch (view) {
    case ReadingStatsActivity::View::Books: return tr(STR_BOOKS);
    case ReadingStatsActivity::View::Calendar: return tr(STR_READING_HEATMAP);
    case ReadingStatsActivity::View::DayDetail: return tr(STR_READING_DAY);
    case ReadingStatsActivity::View::BookDetail: return tr(STR_BOOK_DETAILS);
    case ReadingStatsActivity::View::Profile: return tr(STR_READING_PROFILE);
    case ReadingStatsActivity::View::Achievements: return tr(STR_ACHIEVEMENTS);
    case ReadingStatsActivity::View::Overview:
    default: return tr(STR_READING_STATS);
  }
}

std::string duration(const uint64_t ms) { return ReadingStatsAnalytics::formatDurationHm(ms); }

std::string dateForOrdinal(const uint32_t ordinal) {
  return ReadingStatsAnalytics::formatDayOrdinalLabel(ordinal);
}

uint32_t monthDayOrdinal(const int year, const unsigned month, const unsigned day) {
  return TimeUtils::getDayOrdinalForDate(year, month, day);
}

void moveSelectedCalendarMonth(uint32_t& selectedOrdinal, const int delta) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 1;
  if (!TimeUtils::getDateFromDayOrdinal(selectedOrdinal, year, month, day)) return;
  int nextMonth = static_cast<int>(month) + delta;
  while (nextMonth < 1) { nextMonth += 12; --year; }
  while (nextMonth > 12) { nextMonth -= 12; ++year; }
  day = std::min(day, TimeUtils::getDaysInMonth(year, static_cast<unsigned>(nextMonth)));
  selectedOrdinal = monthDayOrdinal(year, static_cast<unsigned>(nextMonth), day);
}

void moveSelectedCalendarDay(uint32_t& selectedOrdinal, const int delta) {
  if (!selectedOrdinal) return;
  if (delta < 0 && selectedOrdinal > 0) --selectedOrdinal;
  if (delta > 0) ++selectedOrdinal;
}

const ReadingDayStats* findDay(const uint32_t ordinal) {
  for (const auto& day : READING_STATS.getReadingDays()) if (day.dayOrdinal == ordinal) return &day;
  return nullptr;
}

uint64_t dayReading(const uint32_t ordinal) {
  const auto* day = findDay(ordinal);
  return day ? day->readingMs : 0;
}

void drawProgressBar(const GfxRenderer& renderer, int x, int y, int width, int height, uint64_t value, uint64_t max) {
  renderer.drawRect(x, y, width, height);
  const int fill = max == 0 ? 0 : static_cast<int>(std::min<uint64_t>(width - 2, (value * (width - 2)) / max));
  if (fill > 0) renderer.fillRect(x + 1, y + 1, fill, height - 2);
}

void drawMetric(const GfxRenderer& renderer, int x, int y, int width, const char* label, const std::string& value) {
  renderer.drawText(UI_10_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
  renderer.drawText(UI_10_FONT_ID, x + width - valueWidth, y, value.c_str());
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_FUI_ROW, &ReadingStatsActivity::onFuiRow, this);
  app.setScreen(&ReadingStatsActivity::fuiScreen, this);
  fuiNav_.reset();
  READING_STATS.loadFromFile();
  ACHIEVEMENTS.loadFromFile();
  ACHIEVEMENTS.reconcileFromCurrentStats();
  view = View::Overview;
  selectedIndex = 0;
  selectedDayOrdinal = READING_STATS.getReferenceDayOrdinal();
  if (!selectedDayOrdinal && !READING_STATS.getReadingDays().empty()) selectedDayOrdinal = READING_STATS.getReadingDays().back().dayOrdinal;
  selectedBookPath.clear();
  requestUpdate(true);
}

void ReadingStatsActivity::onExit() {
  closeRouting();
  fuiRows_.clear();
  fuiLabels_.clear();
  fuiValues_.clear();
  Activity::onExit();
}

void ReadingStatsActivity::fuiScreen(UiScreen& screen, void* user) {
  static_cast<ReadingStatsActivity*>(user)->buildFuiScreen(screen);
}

void ReadingStatsActivity::onFuiRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReadingStatsActivity*>(user);
  if (event.value < 0) return;

  if (self->view == View::Books) {
    const auto& books = READING_STATS.getBooks();
    if (event.value >= static_cast<int>(books.size())) return;
    self->selectedIndex = event.value;
    self->selectedBookPath = books[static_cast<size_t>(event.value)].path;
    self->view = View::BookDetail;
  } else if (self->view == View::DayDetail) {
    const auto entries = ReadingStatsAnalytics::getBooksReadOnDay(self->selectedDayOrdinal);
    if (event.value >= static_cast<int>(entries.size()) || !entries[static_cast<size_t>(event.value)].book) return;
    self->selectedIndex = event.value;
    self->selectedBookPath = entries[static_cast<size_t>(event.value)].book->path;
    self->view = View::BookDetail;
  } else if (self->view == View::Achievements) {
    const auto views = ACHIEVEMENTS.buildViews();
    if (event.value >= static_cast<int>(views.size())) return;
    self->selectedIndex = event.value;
  }

  self->fuiNav_.reset(self->selectedIndex);
  self->closeRouting();
  self->app.clearTapFlash();
  self->requestUpdate();
}

void ReadingStatsActivity::buildFuiScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                       static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fuiLabels_.clear();
  fuiValues_.clear();

  if (view == View::Books) {
    const auto& books = READING_STATS.getBooks();
    fuiLabels_.reserve(books.size());
    fuiValues_.reserve(books.size());
    for (const auto& book : books) {
      fuiLabels_.push_back(book.title.empty() ? book.path : book.title);
      fuiValues_.push_back(duration(book.totalReadingMs) + "  " + std::to_string(book.lastProgressPercent) + "%");
    }
  } else if (view == View::DayDetail) {
    const auto entries = ReadingStatsAnalytics::getBooksReadOnDay(selectedDayOrdinal);
    fuiLabels_.reserve(entries.size());
    fuiValues_.reserve(entries.size());
    for (const auto& entry : entries) {
      fuiLabels_.push_back(entry.book ? (entry.book->title.empty() ? entry.book->path : entry.book->title) : "-");
      fuiValues_.push_back(duration(entry.readingMs));
    }
  } else if (view == View::Achievements) {
    const auto views = ACHIEVEMENTS.buildViews();
    fuiLabels_.reserve(views.size());
    fuiValues_.reserve(views.size());
    for (const auto& item : views) {
      if (!item.definition) {
        fuiLabels_.emplace_back("-");
        fuiValues_.emplace_back();
        continue;
      }
      fuiLabels_.push_back(item.definition->title);
      const bool durationAchievement = item.definition->metric == AchievementMetric::TotalReadingMs ||
                                       item.definition->metric == AchievementMetric::MaxSessionMs;
      fuiValues_.push_back(durationAchievement
                               ? duration(item.progress) + " / " + duration(item.definition->target)
                               : std::to_string(item.progress) + " / " + std::to_string(item.definition->target));
    }
  }

  const int count = static_cast<int>(fuiLabels_.size());
  if (count <= 0) {
    screen.centeredText(tr(STR_NO_READING_STATS), screen.theme().bodyText);
    return;
  }

  fuiRows_.clear();
  fuiRows_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    fui::ListItem item;
    item.label = fuiLabels_[static_cast<size_t>(i)].c_str();
    item.value = fuiValues_[static_cast<size_t>(i)].empty() ? nullptr : fuiValues_[static_cast<size_t>(i)].c_str();
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
  selectedIndex = std::clamp(selectedIndex, 0, count - 1);
  fuiNav_.selected = selectedIndex;
  fuiNav_.syncToProps(screen.body(), props.rowHeight, screen.theme().listRowGap, count, props);
  screen.list(props);
}

bool ReadingStatsActivity::routeFuiTouch() {
  if (view != View::Books && view != View::DayDetail && view != View::Achievements) return false;

  // renderUi() rebuilds the FUI interaction table while also rebuilding the
  // backing strings in fuiLabels_/fuiValues_.  Serialize routing with render
  // so a tap cannot read a ListItem whose c_str() storage is being replaced.
  RenderLock lock(*this);
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed) {
    const bool invalidated = app.invalidated();
    const bool handled = static_cast<bool>(route.event);
    lock.unlock();
    if (invalidated) requestUpdate();
    return handled;
  }

  // Keep the selection stable while a finger swipe only moves the list
  // viewport.  This mirrors UiListActivity's paging semantics for stats
  // lists, which can contain more rows than fit on the e-paper display.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    int count = 0;
    if (view == View::Books) count = static_cast<int>(READING_STATS.getBooks().size());
    else if (view == View::DayDetail) count = static_cast<int>(ReadingStatsAnalytics::getBooksReadOnDay(selectedDayOrdinal).size());
    else count = static_cast<int>(ACHIEVEMENTS.buildViews().size());
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? fuiNav_.pageRows() : -fuiNav_.pageRows();
    const bool moved = fuiNav_.scrollBy(delta, count);
    lock.unlock();
    if (moved) requestUpdate();
    return true;
  }
  lock.unlock();
  return false;
}

void ReadingStatsActivity::moveVertical(const int delta) {
  RenderLock lock(*this);
  int count = 0;
  if (view == View::Books) count = static_cast<int>(READING_STATS.getBooks().size());
  else if (view == View::DayDetail) count = static_cast<int>(ReadingStatsAnalytics::getBooksReadOnDay(selectedDayOrdinal).size());
  else if (view == View::Achievements) count = static_cast<int>(ACHIEVEMENTS.buildViews().size());
  if (count <= 0) return;
  selectedIndex = delta > 0 ? ButtonNavigator::nextIndex(selectedIndex, count)
                            : ButtonNavigator::previousIndex(selectedIndex, count);
  lock.unlock();
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  if (routeFuiTouch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    bool changed = false;
    bool shouldFinish = false;
    {
      RenderLock lock(*this);
      if (view == View::BookDetail) { view = View::Books; changed = true; }
      else if (view == View::DayDetail) { view = View::Calendar; changed = true; }
      else if (view != View::Overview) { view = View::Overview; changed = true; }
      else shouldFinish = true;
    }
    if (shouldFinish) {
      finish();
      return;
    }
    if (changed) requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock(*this);
      if (view == View::Overview) view = View::Books;
      else if (view == View::Books && !READING_STATS.getBooks().empty()) {
        const auto& books = READING_STATS.getBooks();
        selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(books.size()) - 1);
        selectedBookPath = books[static_cast<size_t>(selectedIndex)].path;
        view = View::BookDetail;
      } else if (view == View::Calendar && selectedDayOrdinal) {
        view = View::DayDetail;
        selectedIndex = 0;
      } else if (view == View::DayDetail) {
        const auto entries = ReadingStatsAnalytics::getBooksReadOnDay(selectedDayOrdinal);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size()) && entries[selectedIndex].book) {
          selectedBookPath = entries[selectedIndex].book->path;
          view = View::BookDetail;
        }
      }
    }
    requestUpdate();
    return;
  }

  if (view == View::Calendar) {
    int calendarDelta = 0;
    bool monthDelta = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) calendarDelta = -1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) calendarDelta = 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) { calendarDelta = -1; monthDelta = true; }
    else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) { calendarDelta = 1; monthDelta = true; }
    if (calendarDelta != 0) {
      {
        RenderLock lock(*this);
        if (monthDelta) moveSelectedCalendarMonth(selectedDayOrdinal, calendarDelta);
        else moveSelectedCalendarDay(selectedDayOrdinal, calendarDelta);
      }
      requestUpdate();
      return;
    }
  } else if (view == View::Overview) {
    View nextView = view;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) nextView = View::Achievements;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) nextView = View::Calendar;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) nextView = View::Profile;
    if (nextView != view) {
      {
        RenderLock lock(*this);
        view = nextView;
      }
      requestUpdate();
      return;
    }
  }

  buttonNavigator.onNextRelease([this] { moveVertical(1); });
  buttonNavigator.onPreviousRelease([this] { moveVertical(-1); });
}

void ReadingStatsActivity::drawFooter(const char* confirmLabel) const {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ReadingStatsActivity::renderOverview() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - x * 2;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const uint64_t today = READING_STATS.getTodayReadingMs();
  const uint64_t goal = getDailyReadingGoalMs();

  drawMetric(renderer, x, top, width, tr(STR_READING_TOTAL), duration(READING_STATS.getTotalReadingMs()));
  drawMetric(renderer, x, top + 28, width, tr(STR_TODAY_GOAL), duration(today) + " / " + duration(goal));
  drawProgressBar(renderer, x, top + 52, width, 13, today, goal);
  drawMetric(renderer, x, top + 82, width / 2 - 8, tr(STR_CURRENT_STREAK), std::to_string(READING_STATS.getCurrentStreakDays()) + "d");
  drawMetric(renderer, x + width / 2, top + 82, width / 2, tr(STR_BEST_STREAK), std::to_string(READING_STATS.getMaxStreakDays()) + "d");
  drawMetric(renderer, x, top + 110, width / 2 - 8, tr(STR_BOOKS_STARTED), std::to_string(READING_STATS.getBooksStartedCount()));
  drawMetric(renderer, x + width / 2, top + 110, width / 2, tr(STR_BOOKS_FINISHED), std::to_string(READING_STATS.getBooksFinishedCount()));

  renderer.drawText(UI_10_FONT_ID, x, top + 150, tr(STR_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  const int chartX = x;
  const int chartY = top + 170;
  const int barWidth = std::max(8, (width - 12) / 7);
  uint64_t maxValue = 1;
  for (int i = 0; i < 7; ++i) maxValue = std::max(maxValue, dayReading(READING_STATS.getReferenceDayOrdinal() >= 6 ? READING_STATS.getReferenceDayOrdinal() - 6 + i : i));
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = READING_STATS.getReferenceDayOrdinal() >= 6 ? READING_STATS.getReferenceDayOrdinal() - 6 + i : i;
    const int h = static_cast<int>(dayReading(ordinal) * 75 / maxValue);
    renderer.fillRect(chartX + i * barWidth, chartY + 75 - h, barWidth - 3, std::max(2, h));
  }
  drawMetric(renderer, x, chartY + 90, width, tr(STR_RECENT_30_DAYS), duration(READING_STATS.getRecentReadingMs(30)));
  renderer.drawText(UI_10_FONT_ID, x, chartY + 124, tr(STR_ANNUAL_BY_MONTH), true, EpdFontFamily::BOLD);
  const int referenceYear = ReadingStatsAnalytics::getReferenceYear();
  uint64_t monthly[12] = {};
  for (const auto& day : READING_STATS.getReadingDays()) {
    int year = 0;
    unsigned month = 0;
    unsigned dayOfMonth = 0;
    if (TimeUtils::getDateFromDayOrdinal(day.dayOrdinal, year, month, dayOfMonth) && year == referenceYear && month >= 1 && month <= 12)
      monthly[month - 1] += day.readingMs;
  }
  uint64_t annualMax = 1;
  for (const auto value : monthly) annualMax = std::max(annualMax, value);
  const int monthBarWidth = std::max(8, (width - 11) / 12);
  for (int month = 0; month < 12; ++month) {
    const int barHeight = static_cast<int>(monthly[month] * 45 / annualMax);
    const int barX = x + month * monthBarWidth;
    renderer.fillRect(barX, chartY + 174 - barHeight, monthBarWidth - 2, std::max(2, barHeight));
    char label[4];
    snprintf(label, sizeof(label), "%02d", month + 1);
    renderer.drawText(SMALL_FONT_ID, barX, chartY + 180, label);
  }
  drawFooter(tr(STR_BOOKS));
}

void ReadingStatsActivity::renderCalendar() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int cell = std::min(48, static_cast<int>((renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / 7));
  const int left = (renderer.getScreenWidth() - cell * 7) / 2;
  if (!selectedDayOrdinal) {
    selectedDayOrdinal = READING_STATS.getReferenceDayOrdinal();
    if (!selectedDayOrdinal) selectedDayOrdinal = TimeUtils::getDayOrdinalForDate(ReadingStatsAnalytics::getReferenceYear(), 1, 1);
  }
  int year = 0;
  unsigned month = 0;
  unsigned day = 1;
  TimeUtils::getDateFromDayOrdinal(selectedDayOrdinal, year, month, day);
  (void)day;
  const uint32_t firstDay = monthDayOrdinal(year, month, 1);
  const unsigned daysInMonth = TimeUtils::getDaysInMonth(year, month);
  const int firstWeekday = static_cast<int>((firstDay + 4) % 7);  // 1970-01-01 was Thursday.
  char monthTitle[16];
  snprintf(monthTitle, sizeof(monthTitle), "%04d-%02u", year, month);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top, monthTitle, true, EpdFontFamily::BOLD);
  static constexpr const char* WEEKDAYS[] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int weekday = 0; weekday < 7; ++weekday) {
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, WEEKDAYS[weekday]);
    renderer.drawText(SMALL_FONT_ID, left + weekday * cell + (cell - textWidth) / 2, top + 20, WEEKDAYS[weekday], true);
  }
  for (int i = 0; i < 42; ++i) {
    const int dayNumber = i - firstWeekday + 1;
    if (dayNumber < 1 || dayNumber > static_cast<int>(daysInMonth)) continue;
    const uint32_t ordinal = firstDay + static_cast<uint32_t>(dayNumber - 1);
    const int x = left + (i % 7) * cell;
    const int y = top + 30 + (i / 7) * 42;
    const uint64_t value = dayReading(ordinal);
    const bool selected = ordinal == selectedDayOrdinal;
    if (selected) renderer.fillRect(x + 1, y + 1, cell - 3, 31, true);
    else if (value >= getDailyReadingGoalMs()) renderer.fillRectDither(x + 1, y + 1, cell - 3, 31, Color::DarkGray);
    else if (value > 0) renderer.fillRectDither(x + 1, y + 1, cell - 3, 31, Color::LightGray);
    renderer.drawRect(x + 1, y + 1, cell - 3, 31, !selected);
    char dayText[8];
    snprintf(dayText, sizeof(dayText), "%02d", dayNumber);
    renderer.drawCenteredText(SMALL_FONT_ID, y + 10, dayText, !selected);
  }
  const int infoY = top + 30 + 6 * 42 + 8;
  drawMetric(renderer, metrics.contentSidePadding, infoY, renderer.getScreenWidth() - 2 * metrics.contentSidePadding,
             tr(STR_SELECTED_DAY), dateForOrdinal(selectedDayOrdinal) + "  " + duration(dayReading(selectedDayOrdinal)));
  drawFooter(tr(STR_READING_DAY));
}

void ReadingStatsActivity::renderDayDetail() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto entries = ReadingStatsAnalytics::getBooksReadOnDay(selectedDayOrdinal);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top, dateForOrdinal(selectedDayOrdinal).c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + 25,
                    (duration(dayReading(selectedDayOrdinal)) + " " + tr(STR_READING_TOTAL)).c_str());
  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + 65, tr(STR_NO_BOOK_REACHED));
  } else {
    const int pageItems = std::max(1, static_cast<int>((renderer.getScreenHeight() - top - 95 - metrics.buttonHintsHeight) / 35));
    const int start = (selectedIndex / pageItems) * pageItems;
    for (int i = start; i < static_cast<int>(entries.size()) && i < start + pageItems; ++i) {
      const int y = top + 65 + (i - start) * 35;
      const bool selected = i == selectedIndex;
      if (selected) renderer.fillRect(0, y - 3, renderer.getScreenWidth(), 30, true);
      const std::string title = entries[i].book ? entries[i].book->title : "";
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, renderer.truncatedText(UI_10_FONT_ID, title.c_str(), 300).c_str(), !selected);
      const std::string value = duration(entries[i].readingMs);
      renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - metrics.contentSidePadding - renderer.getTextWidth(UI_10_FONT_ID, value.c_str()), y, value.c_str(), !selected);
    }
  }
  drawFooter("Back");
}

void ReadingStatsActivity::renderBookDetail() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto* book = READING_STATS.findBook(selectedBookPath);
  if (!book) { renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top, "Book not found"); drawFooter("Back"); return; }
  const int width = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  const std::string title = book->title.empty() ? book->path : book->title;
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, top, renderer.truncatedText(UI_12_FONT_ID, title.c_str(), width).c_str(), true, EpdFontFamily::BOLD);
  if (!book->author.empty()) renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + 28, book->author.c_str());
  const int infoY = top + (book->author.empty() ? 35 : 58);
  char progress[64];
  snprintf(progress, sizeof(progress), "%u%%  %s", book->lastProgressPercent,
           book->completed ? tr(STR_FINISHED) : tr(STR_IN_PROGRESS));
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, infoY, progress, true, EpdFontFamily::BOLD);
  drawProgressBar(renderer, metrics.contentSidePadding, infoY + 22, width, 13, book->lastProgressPercent, 100);
  drawMetric(renderer, metrics.contentSidePadding, infoY + 52, width, tr(STR_READING_TOTAL), duration(book->totalReadingMs));
  drawMetric(renderer, metrics.contentSidePadding, infoY + 80, width, tr(STR_SESSIONS), std::to_string(book->sessions));
  drawMetric(renderer, metrics.contentSidePadding, infoY + 108, width, tr(STR_LAST_SESSION), duration(book->lastSessionMs));
  drawMetric(renderer, metrics.contentSidePadding, infoY + 136, width, tr(STR_LAST_READ), book->lastReadAt ? TimeUtils::formatDate(book->lastReadAt) : "-");
  if (!book->chapterTitle.empty()) renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, infoY + 164, ("Chapter: " + book->chapterTitle).c_str());
  drawFooter("Back");
}

void ReadingStatsActivity::renderProfile() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int readDays = 0, goalDays = 0;
  const uint32_t reference = READING_STATS.getReferenceDayOrdinal();
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = reference >= static_cast<uint32_t>(6 - i) ? reference - 6 + i : i;
    const uint64_t value = dayReading(ordinal);
    if (value) ++readDays;
    if (value >= getDailyReadingGoalMs()) ++goalDays;
  }
  uint64_t shortSessions = 0, mediumSessions = 0, longSessions = 0;
  for (const auto& session : READING_STATS.getSessionLog()) {
    if (session.dayOrdinal + 6 < reference) continue;
    if (session.sessionMs < 10 * 60 * 1000UL) ++shortSessions;
    else if (session.sessionMs < 30 * 60 * 1000UL) ++mediumSessions;
    else ++longSessions;
  }
  const int habit = std::min(100, readDays * 65 / 7 + goalDays * 35 / 7);
  const int stability = std::min(100, static_cast<int>(READING_STATS.getCurrentStreakDays()) * 14 + 30);
  const int engagement = std::min(100, static_cast<int>((mediumSessions + longSessions) * 20 + readDays * 5));
  const int depth = std::min(100, static_cast<int>(mediumSessions * 15 + longSessions * 25));
  renderer.drawText(UI_12_FONT_ID, x, top, tr(STR_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  drawMetric(renderer, x, top + 40, renderer.getScreenWidth() - 2 * x, tr(STR_HABIT), std::to_string(habit) + "/100");
  drawMetric(renderer, x, top + 70, renderer.getScreenWidth() - 2 * x, tr(STR_STABILITY), std::to_string(stability) + "/100");
  drawMetric(renderer, x, top + 100, renderer.getScreenWidth() - 2 * x, tr(STR_ENGAGEMENT), std::to_string(engagement) + "/100");
  drawMetric(renderer, x, top + 130, renderer.getScreenWidth() - 2 * x, tr(STR_SESSION_DEPTH), std::to_string(depth) + "/100");
  const int radarCenterX = renderer.getScreenWidth() / 2;
  const int radarCenterY = top + 205;
  const int radarRadius = 48;
  for (const int level : {25, 50, 75, 100}) {
    const int r = radarRadius * level / 100;
    const int guideX[] = {radarCenterX, radarCenterX + r, radarCenterX, radarCenterX - r};
    const int guideY[] = {radarCenterY - r, radarCenterY, radarCenterY + r, radarCenterY};
    renderer.drawLine(guideX[0], guideY[0], guideX[1], guideY[1]);
    renderer.drawLine(guideX[1], guideY[1], guideX[2], guideY[2]);
    renderer.drawLine(guideX[2], guideY[2], guideX[3], guideY[3]);
    renderer.drawLine(guideX[3], guideY[3], guideX[0], guideY[0]);
  }
  const int scoreX[] = {radarCenterX, radarCenterX + radarRadius * engagement / 100, radarCenterX,
                        radarCenterX - radarRadius * depth / 100};
  const int scoreY[] = {radarCenterY - radarRadius * habit / 100, radarCenterY,
                        radarCenterY + radarRadius * stability / 100, radarCenterY};
  renderer.drawLine(scoreX[0], scoreY[0], scoreX[1], scoreY[1], 2, true);
  renderer.drawLine(scoreX[1], scoreY[1], scoreX[2], scoreY[2], 2, true);
  renderer.drawLine(scoreX[2], scoreY[2], scoreX[3], scoreY[3], 2, true);
  renderer.drawLine(scoreX[3], scoreY[3], scoreX[0], scoreY[0], 2, true);
  renderer.drawCenteredText(SMALL_FONT_ID, radarCenterY - radarRadius - 20, tr(STR_HABIT), true);
  renderer.drawCenteredText(SMALL_FONT_ID, radarCenterY + radarRadius + 8, tr(STR_STABILITY), true);
  const int total = (habit + stability + engagement + depth) / 4;
  renderer.drawText(UI_12_FONT_ID, x, top + 285, ("Overall score: " + std::to_string(total) + "/100").c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, top + 330, ("<10m  " + std::to_string(shortSessions) + "   10-29m  " + std::to_string(mediumSessions) + "   30m+  " + std::to_string(longSessions)).c_str());
  drawFooter("Back");
}

void ReadingStatsActivity::renderAchievements() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto views = ACHIEVEMENTS.buildViews();
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top,
                    (std::to_string(ACHIEVEMENTS.unlockedCount()) + " / " + std::to_string(views.size()) + " " + tr(STR_UNLOCKED)).c_str(), true, EpdFontFamily::BOLD);
  const int listTop = top + 28;
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int pageItems = std::max(1, static_cast<int>((renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight) / rowHeight));
  const int start = (selectedIndex / pageItems) * pageItems;
  for (int i = start; i < static_cast<int>(views.size()) && i < start + pageItems; ++i) {
    const int y = listTop + (i - start) * rowHeight;
    const bool selected = i == selectedIndex;
    if (selected) renderer.fillRect(0, y - 2, renderer.getScreenWidth(), rowHeight, true);
    const auto& item = views[i];
    const std::string title = item.definition->title;
    const bool isDurationAchievement = item.definition->metric == AchievementMetric::TotalReadingMs ||
                                       item.definition->metric == AchievementMetric::MaxSessionMs;
    const std::string progress = isDurationAchievement
                                     ? duration(item.progress) + " / " + duration(item.definition->target)
                                     : std::to_string(item.progress) + " / " + std::to_string(item.definition->target);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, renderer.truncatedText(UI_10_FONT_ID, title.c_str(), 330).c_str(), !selected);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + 22, progress.c_str(), !selected);
  }
  drawFooter("Back");
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, UITheme::getInstance().getMetrics().topPadding, renderer.getScreenWidth(),
                                UITheme::getInstance().getMetrics().headerHeight}, viewTitle(view));
  if (view == View::Books || view == View::DayDetail || view == View::Achievements) {
    renderUi();
    drawFooter(view == View::Books ? tr(STR_BOOK_DETAILS) : "Back");
    renderer.displayBuffer();
    return;
  }
  switch (view) {
    case View::Calendar: renderCalendar(); break;
    case View::DayDetail: renderDayDetail(); break;
    case View::BookDetail: renderBookDetail(); break;
    case View::Profile: renderProfile(); break;
    case View::Achievements: renderAchievements(); break;
    case View::Overview:
    default: renderOverview(); break;
  }
  renderer.displayBuffer();
}
