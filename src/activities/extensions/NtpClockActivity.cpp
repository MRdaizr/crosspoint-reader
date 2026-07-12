#include "NtpClockActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <array>
#include <cstdio>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
constexpr time_t VALID_EPOCH = 1700000000;  // 2023-11-14
constexpr time_t HALF_HOUR_SECONDS = 30 * 60;
constexpr std::array<StrId, 7> WEEKDAYS = {StrId::STR_WEEKDAY_SUN, StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE,
                                            StrId::STR_WEEKDAY_WED, StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI,
                                            StrId::STR_WEEKDAY_SAT};

constexpr int DIGIT_WIDTH = 86;
constexpr int DIGIT_HEIGHT = 190;
constexpr int DIGIT_STROKE = 15;
constexpr int DIGIT_GAP = 12;
constexpr int COLON_WIDTH = 28;

void drawClockDigit(const GfxRenderer& renderer, const int x, const int y, const int digit) {
  static constexpr uint8_t SEGMENTS[] = {
      0b00111111, 0b00000110, 0b01011011, 0b01001111, 0b01100110,
      0b01101101, 0b01111101, 0b00000111, 0b01111111, 0b01101111,
  };
  const uint8_t segments = SEGMENTS[digit];
  const int halfHeight = (DIGIT_HEIGHT - DIGIT_STROKE) / 2;
  const int horizontalWidth = DIGIT_WIDTH - 2 * DIGIT_STROKE;
  const auto draw = [&](const uint8_t segment, const int sx, const int sy, const int width, const int height) {
    if (segments & (1 << segment)) renderer.fillRect(sx, sy, width, height);
  };
  draw(0, x + DIGIT_STROKE, y, horizontalWidth, DIGIT_STROKE);
  draw(1, x + DIGIT_WIDTH - DIGIT_STROKE, y + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(2, x + DIGIT_WIDTH - DIGIT_STROKE, y + halfHeight + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(3, x + DIGIT_STROKE, y + DIGIT_HEIGHT - DIGIT_STROKE, horizontalWidth, DIGIT_STROKE);
  draw(4, x, y + halfHeight + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(5, x, y + DIGIT_STROKE, DIGIT_STROKE, halfHeight);
  draw(6, x + DIGIT_STROKE, y + halfHeight, horizontalWidth, DIGIT_STROKE);
}

void drawClockColon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int DOT = 18;
  renderer.fillRect(x + (COLON_WIDTH - DOT) / 2, y + DIGIT_HEIGHT / 3 - DOT / 2, DOT, DOT);
  renderer.fillRect(x + (COLON_WIDTH - DOT) / 2, y + DIGIT_HEIGHT * 2 / 3 - DOT / 2, DOT, DOT);
}
}  // namespace

bool NtpClockActivity::hasValidSystemTime() { return time(nullptr) >= VALID_EPOCH; }

void NtpClockActivity::onEnter() {
  Activity::onEnter();
  lastSyncFailed = false;
  beginSync();
}

void NtpClockActivity::onExit() {
  Activity::onExit();
  tearDownWifi();
}

void NtpClockActivity::beginSync() {
  lastSyncFailed = false;
  if (WiFi.status() == WL_CONNECTED) {
    state = State::SYNCING;
    requestUpdate();
    return;
  }

  ownsWifi = true;
  launchWifiSelection();
}

void NtpClockActivity::launchWifiSelection() {
  state = State::CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void NtpClockActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected || WiFi.status() != WL_CONNECTED) {
    lastSyncFailed = true;
    state = State::DISPLAYING;
    scheduleNextSync();
    tearDownWifi();
    requestUpdate();
    return;
  }
  state = State::SYNCING;
  requestUpdate();
}

void NtpClockActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("NTC", "WiFi disconnected before NTP sync");
    lastSyncFailed = true;
  } else {
    LOG_INF("NTC", "Starting NTP sync");
    if (esp_sntp_enabled()) esp_sntp_stop();
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.nist.gov");
    esp_sntp_init();
    bool synced = false;
    for (int i = 0; i < 150; ++i) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        synced = true;
        break;
      }
      delay(100);
    }
    lastSyncFailed = !synced || !hasValidSystemTime();
    if (lastSyncFailed) {
      LOG_ERR("NTC", "NTP sync timed out (system time valid=%d)", hasValidSystemTime() ? 1 : 0);
    } else {
      LOG_INF("NTC", "NTP sync complete");
    }
  }

  state = State::DISPLAYING;
  scheduleNextSync();
  tearDownWifi();
  requestUpdate();
}

void NtpClockActivity::scheduleNextSync() {
  const time_t now = time(nullptr);
  if (now >= VALID_EPOCH) {
    nextSyncAt = (now / HALF_HOUR_SECONDS + 1) * HALF_HOUR_SECONDS;
    fallbackSyncAtMs = 0;
  } else {
    // Without a valid clock there is no meaningful wall-clock half hour.
    nextSyncAt = 0;
    fallbackSyncAtMs = millis() + HALF_HOUR_SECONDS * 1000UL;
  }
}

void NtpClockActivity::tearDownWifi() {
  if (!ownsWifi) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ownsWifi = false;
}

void NtpClockActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::SYNCING) {
    requestUpdateAndWait();
    runSync();
    return;
  }

  const time_t now = time(nullptr);
  if ((nextSyncAt != 0 && now >= nextSyncAt) ||
      (nextSyncAt == 0 && fallbackSyncAtMs != 0 && millis() >= fallbackSyncAtMs)) {
    beginSync();
    return;
  }

  if (hasValidSystemTime() && now / 60 != lastDisplayedMinute) {
    lastDisplayedMinute = now / 60;
    requestUpdate();
  }
}

void NtpClockActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NTP_CLOCK));

  if (state == State::CONNECTING || state == State::SYNCING) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_CLOCK_SYNCING));
  } else if (hasValidSystemTime()) {
    const time_t now = time(nullptr);
    struct tm localTime {};
    localtime_r(&now, &localTime);

    char clockText[6];
    snprintf(clockText, sizeof(clockText), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
    const int clockWidth = DIGIT_WIDTH * 4 + DIGIT_GAP * 4 + COLON_WIDTH;
    const int clockX = (pageWidth - clockWidth) / 2;
    const int clockY = pageHeight / 2 - DIGIT_HEIGHT / 2 - 35;
    drawClockDigit(renderer, clockX, clockY, localTime.tm_hour / 10);
    drawClockDigit(renderer, clockX + DIGIT_WIDTH + DIGIT_GAP, clockY, localTime.tm_hour % 10);
    drawClockColon(renderer, clockX + (DIGIT_WIDTH + DIGIT_GAP) * 2, clockY);
    const int minuteX = clockX + (DIGIT_WIDTH + DIGIT_GAP) * 2 + COLON_WIDTH + DIGIT_GAP;
    drawClockDigit(renderer, minuteX, clockY, localTime.tm_min / 10);
    drawClockDigit(renderer, minuteX + DIGIT_WIDTH + DIGIT_GAP, clockY, localTime.tm_min % 10);

    const StrId weekday = WEEKDAYS[static_cast<size_t>(localTime.tm_wday)];
    char dateText[48];
    snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d %s", localTime.tm_year + 1900, localTime.tm_mon + 1,
             localTime.tm_mday, I18N.get(weekday));
    const int dateFont = DynamicFont::fontForCjkText(renderer, dateText, UI_10_FONT_ID);
    DynamicFont::prewarmIfSdFont(renderer, dateFont, dateText);
    renderer.drawCenteredText(dateFont, clockY + DIGIT_HEIGHT + 32, dateText);
    if (lastSyncFailed) {
      renderer.drawCenteredText(UI_10_FONT_ID, clockY + DIGIT_HEIGHT + 62, tr(STR_NTP_CLOCK_USING_RTC));
    }
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 15, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, tr(STR_NTP_CLOCK_USING_RTC));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
