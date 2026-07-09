#include "PomodoroActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  remainingMs = durationMs;
  running = false;
  lastTickMs = millis();
  requestUpdate();
}

void PomodoroActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    running = !running;
    lastTickMs = millis();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    remainingMs = durationMs;
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
    running = false;
  } else {
    remainingMs -= elapsed;
  }
  requestUpdate();
}

void PomodoroActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POMODORO));

  const unsigned long totalSeconds = (remainingMs + 999) / 1000;
  char timeText[16];
  snprintf(timeText, sizeof(timeText), "%02lu:%02lu", totalSeconds / 60, totalSeconds % 60);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 15, timeText, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25,
                            running ? tr(STR_POMODORO_RUNNING) : tr(STR_POMODORO_PAUSED));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_START_PAUSE), tr(STR_RESET), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
