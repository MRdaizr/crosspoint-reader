#include "TimerActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "BigClock.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long MILLIS_PER_SECOND = 1000UL;
constexpr unsigned long MILLIS_PER_MINUTE = 60UL * MILLIS_PER_SECOND;

}  // namespace

void TimerActivity::onEnter() {
  Activity::onEnter();
  mode = Mode::COUNTDOWN;
  state = State::SETUP;
  presetIndex = 3;
  remainingMs = selectedDurationMs();
  elapsedMs = 0;
  completionElapsedMs = 0;
  lastTickMs = 0;
  requestUpdate();
}

unsigned long TimerActivity::selectedDurationMs() const {
  return PRESET_MINUTES[presetIndex] * MILLIS_PER_MINUTE;
}

const char* TimerActivity::modeLabel() const {
  return mode == Mode::COUNTDOWN ? tr(STR_TIMER_COUNTDOWN) : tr(STR_TIMER_STOPWATCH);
}

void TimerActivity::changePreset(const int delta) {
  int next = static_cast<int>(presetIndex) + delta;
  next = std::clamp(next, 0, static_cast<int>(PRESET_COUNT) - 1);
  presetIndex = static_cast<uint8_t>(next);
  remainingMs = selectedDurationMs();
  requestUpdate();
}

void TimerActivity::startTimer() {
  remainingMs = selectedDurationMs();
  elapsedMs = 0;
  completionElapsedMs = 0;
  lastTickMs = millis();
  state = State::RUNNING;
  requestUpdate(true);
}

void TimerActivity::resetTimer() {
  remainingMs = selectedDurationMs();
  elapsedMs = 0;
  completionElapsedMs = 0;
  lastTickMs = 0;
  state = State::PAUSED;
  requestUpdate(true);
}

void TimerActivity::completeTimer() {
  if (mode == Mode::COUNTDOWN) {
    completionElapsedMs = selectedDurationMs() - remainingMs;
  } else {
    completionElapsedMs = elapsedMs;
  }
  state = State::COMPLETION;
  requestUpdate(true);
}

bool TimerActivity::advanceTimer(const unsigned long now) {
  if (state != State::RUNNING) return false;

  const unsigned long delta = now - lastTickMs;
  if (delta == 0) return false;
  lastTickMs = now;

  if (mode == Mode::COUNTDOWN) {
    if (delta >= remainingMs) {
      remainingMs = 0;
      elapsedMs = selectedDurationMs();
      completeTimer();
      return true;
    }
    remainingMs -= delta;
    elapsedMs += delta;
  } else {
    elapsedMs += delta;
  }
  return false;
}

unsigned long TimerActivity::displayedMs() const {
  if (state == State::COMPLETION) return completionElapsedMs;
  return mode == Mode::COUNTDOWN ? remainingMs : elapsedMs;
}

void TimerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == State::SETUP || state == State::COMPLETION) {
      finish();
      return;
    }
    if (state == State::RUNNING) {
      advanceTimer(millis());
      if (state == State::COMPLETION) return;
    }
    completeTimer();
    return;
  }

  if (state == State::SETUP) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      mode = mappedInput.wasReleased(MappedInputManager::Button::Left) ? Mode::COUNTDOWN : Mode::STOPWATCH;
      remainingMs = selectedDurationMs();
      requestUpdate();
      return;
    }
    if (mode == Mode::COUNTDOWN && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      changePreset(1);
      return;
    }
    if (mode == Mode::COUNTDOWN && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      changePreset(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startTimer();
      return;
    }
    return;
  }

  if (state == State::COMPLETION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startTimer();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == State::RUNNING) {
      if (advanceTimer(millis())) return;
      state = State::PAUSED;
      lastTickMs = 0;
    } else {
      state = State::RUNNING;
      lastTickMs = millis();
    }
    requestUpdate(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    resetTimer();
    return;
  }

  if (state != State::RUNNING) return;

  const unsigned long now = millis();
  if (now - lastTickMs < MILLIS_PER_SECOND) return;
  if (advanceTimer(now)) return;
  requestUpdate();
}

void TimerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case State::SETUP:
      renderSetup();
      renderer.displayBuffer();
      break;
    case State::RUNNING:
    case State::PAUSED:
      renderRunning();
      renderer.displayBuffer();
      break;
    case State::COMPLETION:
      renderCompletion();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      break;
  }
}

void TimerActivity::renderSetup() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TIMER));

  const int centerY = pageHeight / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 92, tr(STR_TIMER_MODE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 55, modeLabel(), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 5, tr(STR_TIMER_DURATION), true, EpdFontFamily::BOLD);

  if (mode == Mode::COUNTDOWN) {
    char duration[32];
    snprintf(duration, sizeof(duration), tr(STR_TIMER_MINUTES_FORMAT), PRESET_MINUTES[presetIndex]);
    renderer.drawCenteredText(UI_12_FONT_ID, centerY + 43, duration, true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, centerY + 43, tr(STR_TIMER_UNLIMITED), true, EpdFontFamily::BOLD);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 85, tr(STR_TIMER_MODE_HINT));
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 108, tr(STR_TIMER_DURATION_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_START_PAUSE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TimerActivity::drawTimeValue(const unsigned long timeMs) {
  const unsigned long totalSeconds = timeMs / MILLIS_PER_SECOND;
  const unsigned long totalMinutes = totalSeconds / 60UL;
  if (totalMinutes < 100UL) {
    BigClock::drawTime(renderer, renderer.getScreenWidth(), renderer.getScreenHeight(),
                       static_cast<int>(totalMinutes), static_cast<int>(totalSeconds % 60UL));
    return;
  }

  const unsigned long hours = std::min(99UL, totalMinutes / 60UL);
  BigClock::drawTime(renderer, renderer.getScreenWidth(), renderer.getScreenHeight(), static_cast<int>(hours),
                     static_cast<int>(totalMinutes % 60UL));
}

void TimerActivity::renderRunning() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TIMER));

  drawTimeValue(displayedMs());
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 32, modeLabel());
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 56,
                            state == State::RUNNING ? tr(STR_POMODORO_RUNNING) : tr(STR_POMODORO_PAUSED));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_START_PAUSE), tr(STR_RESET), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TimerActivity::renderCompletion() {
  const int pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, BigClock::top(pageHeight) - 45, tr(STR_TIMER_COMPLETED), true,
                            EpdFontFamily::BOLD);
  drawTimeValue(completionElapsedMs);
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 32,
                            tr(STR_TIMER_ELAPSED));
  renderer.drawCenteredText(UI_10_FONT_ID, BigClock::top(pageHeight) + BigClock::DIGIT_HEIGHT + 56, modeLabel());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TIMER_RESTART), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
