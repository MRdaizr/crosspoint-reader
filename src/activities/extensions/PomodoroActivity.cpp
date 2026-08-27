#include "PomodoroActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <HalDisplay.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "BigClock.h"
#include "PomodoroStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {

constexpr uint32_t DAILY_FOCUS_GOAL_SECONDS = 4UL * 25UL * 60UL;

std::string formatDuration(const uint32_t seconds) {
  const uint32_t minutes = (seconds + 30U) / 60U;
  char text[24];
  if (minutes < 60U) {
    snprintf(text, sizeof(text), "%lum", static_cast<unsigned long>(minutes));
  } else {
    snprintf(text, sizeof(text), "%luh%02lum", static_cast<unsigned long>(minutes / 60U),
             static_cast<unsigned long>(minutes % 60U));
  }
  return text;
}

const PomodoroDailyEntry* findToday(const std::vector<PomodoroDailyEntry>& entries) {
  const std::string today = TimeUtils::formatDate(TimeUtils::getCurrentValidTimestamp());
  if (today.empty()) return nullptr;
  for (const auto& entry : entries) {
    if (entry.date == today) return &entry;
  }
  return nullptr;
}

uint32_t ordinalForDate(const std::string& date) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (date.size() != 10 || sscanf(date.c_str(), "%d-%u-%u", &year, &month, &day) != 3) return 0;
  return TimeUtils::getDayOrdinalForDate(year, month, day);
}

uint32_t focusSecondsForDay(const std::vector<PomodoroDailyEntry>& entries, const uint32_t ordinal) {
  for (const auto& entry : entries) {
    if (ordinalForDate(entry.date) == ordinal) return entry.focusSeconds;
  }
  return 0;
}

uint32_t completedFocusesForDay(const std::vector<PomodoroDailyEntry>& entries, const uint32_t ordinal) {
  for (const auto& entry : entries) {
    if (ordinalForDate(entry.date) == ordinal) return entry.completedFocuses;
  }
  return 0;
}

uint32_t currentStreak(const std::vector<PomodoroDailyEntry>& entries, const uint32_t todayOrdinal) {
  if (!todayOrdinal) return 0;
  uint32_t streak = 0;
  for (uint32_t ordinal = todayOrdinal;; --ordinal) {
    if (!completedFocusesForDay(entries, ordinal)) break;
    ++streak;
    if (!ordinal) break;
  }
  return streak;
}

uint32_t bestStreak(const std::vector<PomodoroDailyEntry>& entries) {
  std::vector<uint32_t> ordinals;
  ordinals.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.completedFocuses) ordinals.push_back(ordinalForDate(entry.date));
  }
  std::sort(ordinals.begin(), ordinals.end());
  ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());

  uint32_t best = 0;
  uint32_t run = 0;
  uint32_t previous = 0;
  for (const uint32_t ordinal : ordinals) {
    run = run && ordinal == previous + 1 ? run + 1 : 1;
    best = std::max(best, run);
    previous = ordinal;
  }
  return best;
}

void drawMetric(const GfxRenderer& renderer, const int x, const int y, const int width, const char* label,
                const std::string& value) {
  renderer.drawText(UI_10_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
  renderer.drawText(UI_10_FONT_ID, x + width - valueWidth, y, value.c_str());
}

void drawProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                     const uint64_t value, const uint64_t maximum) {
  renderer.drawRect(x, y, width, height);
  const int fill = maximum == 0 ? 0 : static_cast<int>(std::min<uint64_t>(width - 2, (value * (width - 2)) / maximum));
  if (fill > 0) renderer.fillRect(x + 1, y + 1, fill, height - 2);
}

}  // namespace

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  completedFocusesInCycle = 0;
  showingStats = false;
  startPhase(Phase::FOCUS, false);
  requestUpdate();
}

void PomodoroActivity::startPhase(const Phase newPhase, const bool startImmediately) {
  phase = newPhase;
  switch (phase) {
    case Phase::FOCUS:
      remainingMs = FOCUS_DURATION_MS;
      break;
    case Phase::SHORT_BREAK:
      remainingMs = SHORT_BREAK_DURATION_MS;
      break;
    case Phase::LONG_BREAK:
      remainingMs = LONG_BREAK_DURATION_MS;
      break;
    case Phase::COMPLETION:
      break;
  }
  running = startImmediately;
  lastTickMs = millis();
}

const char* PomodoroActivity::phaseLabel() const {
  switch (phase) {
    case Phase::FOCUS:
      return tr(STR_POMODORO_FOCUS);
    case Phase::SHORT_BREAK:
      return tr(STR_POMODORO_SHORT_BREAK);
    case Phase::LONG_BREAK:
      return tr(STR_POMODORO_LONG_BREAK);
    case Phase::COMPLETION:
      return "";
  }
  return "";
}

const char* PomodoroActivity::completionLabel() const {
  switch (nextPhase) {
    case Phase::SHORT_BREAK:
      return tr(STR_POMODORO_FOCUS_COMPLETE);
    case Phase::LONG_BREAK:
      return tr(STR_POMODORO_CYCLE_COMPLETE);
    case Phase::FOCUS:
      return tr(STR_POMODORO_BREAK_COMPLETE);
    case Phase::COMPLETION:
      return "";
  }
  return "";
}

void PomodoroActivity::completeCurrentPhase() {
  if (phase == Phase::FOCUS) {
    POMODORO_STATS.recordCompletedFocus(FOCUS_DURATION_MS / 1000UL);
    completedFocusesInCycle++;
    nextPhase = completedFocusesInCycle >= 4 ? Phase::LONG_BREAK : Phase::SHORT_BREAK;
  } else if (phase == Phase::LONG_BREAK) {
    completedFocusesInCycle = 0;
    nextPhase = Phase::FOCUS;
  } else {
    nextPhase = Phase::FOCUS;
  }

  phase = Phase::COMPLETION;
  running = false;
  completionEndsAtMs = millis() + COMPLETION_NOTICE_MS;
  requestUpdate(true);
}

void PomodoroActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (showingStats) {
      showingStats = false;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (phase == Phase::COMPLETION) {
    if (static_cast<long>(millis() - completionEndsAtMs) >= 0) {
      startPhase(nextPhase, true);
      requestUpdate();
    }
    return;
  }

  if (showingStats) return;

  if (!running && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    showingStats = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    running = !running;
    lastTickMs = millis();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    startPhase(phase, false);
    running = false;
    requestUpdate();
    return;
  }

  if (!running) return;
  const unsigned long now = millis();
  const unsigned long elapsed = now - lastTickMs;
  if (elapsed < 1000) return;
  lastTickMs = now;
  if (elapsed >= remainingMs) {
    remainingMs = 0;
    completeCurrentPhase();
  } else {
    const unsigned long previousMinutes = (remainingMs + 59999UL) / 60000UL;
    remainingMs -= elapsed;
    const unsigned long currentMinutes = (remainingMs + 59999UL) / 60000UL;
    if (currentMinutes != previousMinutes) requestUpdate();
  }
}

void PomodoroActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (phase == Phase::COMPLETION) {
    renderCompletion();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  if (showingStats) {
    renderStats();
    renderer.displayBuffer();
    return;
  }

  renderTimer();
  renderer.displayBuffer();
}

void PomodoroActivity::renderTimer() {

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POMODORO));

  const unsigned long remainingMinutes = (remainingMs + 59999UL) / 60000UL;
  BigClock::drawTime(renderer, pageWidth, pageHeight, static_cast<int>(remainingMinutes), 0);
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 32,
                            phaseLabel());
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 56,
                            running ? tr(STR_POMODORO_RUNNING) : tr(STR_POMODORO_PAUSED));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_START_PAUSE), tr(STR_RESET),
                                            running ? "" : tr(STR_POMODORO_STATS));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PomodoroActivity::renderCompletion() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 45, completionLabel(), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, tr(STR_POMODORO_NEXT_PHASE));
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + 35,
                            nextPhase == Phase::FOCUS ? tr(STR_POMODORO_FOCUS) :
                            nextPhase == Phase::SHORT_BREAK ? tr(STR_POMODORO_SHORT_BREAK) : tr(STR_POMODORO_LONG_BREAK),
                            true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PomodoroActivity::drawStatsFooter() const {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PomodoroActivity::renderStats() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POMODORO_STATS));

  const auto& entries = POMODORO_STATS.getRecentDailyEntries();
  const bool hasValidDate = POMODORO_STATS.hasValidDate();
  const int x = metrics.contentSidePadding;
  const int width = pageWidth - x * 2;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const uint32_t totalSeconds = POMODORO_STATS.getTotalFocusSeconds();

  drawMetric(renderer, x, top, width, tr(STR_READING_TOTAL), formatDuration(totalSeconds));

  if (!hasValidDate) {
    renderer.drawText(UI_10_FONT_ID, x, top + 32, tr(STR_POMODORO_TIME_UNCALIBRATED));
    drawStatsFooter();
    return;
  }

  const PomodoroDailyEntry* today = findToday(entries);
  const uint32_t todayOrdinal = TimeUtils::getLocalDayOrdinal(TimeUtils::getCurrentValidTimestamp());
  const uint32_t todaySeconds = today ? today->focusSeconds : 0;
  drawMetric(renderer, x, top + 28, width, tr(STR_TODAY_GOAL),
             formatDuration(todaySeconds) + " / " + formatDuration(DAILY_FOCUS_GOAL_SECONDS));
  drawProgressBar(renderer, x, top + 52, width, 13, todaySeconds, DAILY_FOCUS_GOAL_SECONDS);
  drawMetric(renderer, x, top + 82, width / 2 - 8, tr(STR_CURRENT_STREAK),
             std::to_string(currentStreak(entries, todayOrdinal)) + "d");
  drawMetric(renderer, x + width / 2, top + 82, width / 2, tr(STR_BEST_STREAK),
             std::to_string(bestStreak(entries)) + "d");

  renderer.drawText(UI_10_FONT_ID, x, top + 150, tr(STR_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  const int chartX = x;
  const int chartY = top + 170;
  const int barWidth = std::max(8, (width - 12) / 7);
  uint32_t maxSeconds = 1;
  const uint32_t firstRecentDay = todayOrdinal >= 6 ? todayOrdinal - 6 : 0;
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = firstRecentDay + static_cast<uint32_t>(i);
    maxSeconds = std::max(maxSeconds, focusSecondsForDay(entries, ordinal));
  }
  for (int i = 0; i < 7; ++i) {
    const uint32_t ordinal = firstRecentDay + static_cast<uint32_t>(i);
    const int height = static_cast<int>(focusSecondsForDay(entries, ordinal) * 75ULL / maxSeconds);
    renderer.fillRect(chartX + i * barWidth, chartY + 75 - height, barWidth - 3, std::max(2, height));
  }

  uint32_t recentSeconds = 0;
  const uint32_t firstThirtyDay = todayOrdinal >= 29 ? todayOrdinal - 29 : 0;
  for (const auto& entry : entries) {
    const uint32_t ordinal = ordinalForDate(entry.date);
    if (ordinal >= firstThirtyDay && ordinal <= todayOrdinal) recentSeconds += entry.focusSeconds;
  }
  drawMetric(renderer, x, chartY + 90, width, tr(STR_RECENT_30_DAYS), formatDuration(recentSeconds));

  renderer.drawText(UI_10_FONT_ID, x, chartY + 124, tr(STR_ANNUAL_BY_MONTH), true, EpdFontFamily::BOLD);
  int referenceYear = 0;
  unsigned referenceMonth = 0;
  unsigned referenceDay = 0;
  TimeUtils::getDateFromDayOrdinal(todayOrdinal, referenceYear, referenceMonth, referenceDay);
  uint32_t monthly[12] = {};
  for (const auto& entry : entries) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    const uint32_t ordinal = ordinalForDate(entry.date);
    if (!ordinal || !TimeUtils::getDateFromDayOrdinal(ordinal, year, month, day)) continue;
    if (year == referenceYear && month >= 1 && month <= 12) monthly[month - 1] += entry.focusSeconds;
  }
  uint32_t annualMax = 1;
  for (const auto value : monthly) annualMax = std::max(annualMax, value);
  const int monthBarWidth = std::max(8, (width - 11) / 12);
  for (int month = 0; month < 12; ++month) {
    const int barHeight = static_cast<int>(monthly[month] * 45ULL / annualMax);
    const int barX = x + month * monthBarWidth;
    renderer.fillRect(barX, chartY + 174 - barHeight, monthBarWidth - 2, std::max(2, barHeight));
    char label[4];
    snprintf(label, sizeof(label), "%02d", month + 1);
    renderer.drawText(SMALL_FONT_ID, barX, chartY + 180, label);
  }

  drawStatsFooter();
}
