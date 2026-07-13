#include "PomodoroActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <HalDisplay.h>

#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "BigClock.h"
#include "PomodoroStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

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
  const time_t now = time(nullptr);
  struct tm localTime {};
  localtime_r(&now, &localTime);
  char today[11];
  snprintf(today, sizeof(today), "%04d-%02d-%02d", localTime.tm_year + 1900, localTime.tm_mon + 1,
           localTime.tm_mday);
  for (const auto& entry : entries) {
    if (entry.date == today) return &entry;
  }
  return nullptr;
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

void PomodoroActivity::renderStats() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POMODORO_STATS));

  const auto& entries = POMODORO_STATS.getRecentDailyEntries();
  const int left = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;
  char line[64];
  const bool hasValidDate = POMODORO_STATS.hasValidDate();
  if (hasValidDate) {
    const PomodoroDailyEntry* today = findToday(entries);
    snprintf(line, sizeof(line), "%s: %lu  %s", tr(STR_POMODORO_TODAY),
             static_cast<unsigned long>(today ? today->completedFocuses : 0),
             formatDuration(today ? today->focusSeconds : 0).c_str());
    renderer.drawText(UI_12_FONT_ID, left, y, line, true, EpdFontFamily::BOLD);
  } else {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_POMODORO_TIME_UNCALIBRATED));
  }
  y += 34;
  snprintf(line, sizeof(line), "%s: %lu  %s", tr(STR_POMODORO_TOTAL),
           static_cast<unsigned long>(POMODORO_STATS.getTotalCompletedFocuses()),
           formatDuration(POMODORO_STATS.getTotalFocusSeconds()).c_str());
  renderer.drawText(UI_10_FONT_ID, left, y, line, true, EpdFontFamily::BOLD);
  y += 38;
  if (!hasValidDate) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_POMODORO_LAST_SEVEN_DAYS), true, EpdFontFamily::BOLD);
  y += 24;
  for (const auto& entry : entries) {
    snprintf(line, sizeof(line), "%s   %lu  %s", entry.date.c_str(), static_cast<unsigned long>(entry.completedFocuses),
             formatDuration(entry.focusSeconds).c_str());
    renderer.drawText(UI_10_FONT_ID, left, y, line);
    y += 23;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
