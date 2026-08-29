#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;

namespace {
constexpr time_t MIN_TRUSTED_EPOCH = 1704016800;  // 2024-01-01 at UTC+14
}

void HalClock::begin() {
  // FreeInk selects the active board profile before this call. The SDK RTC
  // wrapper handles the X3 DS3231 bus and reports unavailable on X4.
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  if (_available) {
    uint8_t hour = 0;
    uint8_t minute = 0;
    getTime(hour, minute);
  }
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dateTime;
  if (!_sdkRtc.now(dateTime)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedHour = dateTime.hour;
  _cachedMinute = dateTime.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h = 0;
  uint8_t m = 0;
  if (!getTime(h, m)) return false;

  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  if (!_available || hour >= 24 || minute >= 60 || second >= 60) return false;

  Rtc::DateTime dateTime;
  if (!_sdkRtc.now(dateTime)) return false;
  dateTime.hour = hour;
  dateTime.minute = minute;
  dateTime.second = second;
  if (!_sdkRtc.set(dateTime)) {
    LOG_ERR("CLK", "Failed to write time through FreeInk RTC");
    return false;
  }

  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.aliyun.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_setservername(2, "time.nist.gov");
  esp_sntp_init();

  constexpr int maxAttempts = 150;
  for (int i = 0; i < maxAttempts; ++i) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && hasValidTime()) {
      const time_t current = time(nullptr);
      struct tm timeInfo {};
      gmtime_r(&current, &timeInfo);

      if (_available && writeTimeToRTC(static_cast<uint8_t>(timeInfo.tm_hour), static_cast<uint8_t>(timeInfo.tm_min),
                                       static_cast<uint8_t>(timeInfo.tm_sec))) {
        LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
      } else if (_available) {
        LOG_ERR("CLK", "System time synced, but RTC write failed");
      } else {
        LOG_INF("CLK", "System time synced (no RTC on this device)");
      }
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

time_t HalClock::nowUtc() const {
  const time_t now = time(nullptr);
  return now >= MIN_TRUSTED_EPOCH ? now : 0;
}

bool HalClock::hasValidTime() const { return nowUtc() != 0; }

bool HalClock::requestSync() {
  _syncState = ClockSyncState::Syncing;
  const bool ok = syncFromNTP();
  _syncState = ok ? ClockSyncState::Succeeded : ClockSyncState::Failed;
  return ok;
}

bool HalClock::syncNow(uint32_t /*timeoutMs*/) { return requestSync(); }
