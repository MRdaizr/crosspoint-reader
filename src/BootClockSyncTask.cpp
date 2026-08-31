#include "BootClockSyncTask.h"

#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

#include "WifiCredentialStore.h"
#include "util/TimeUtils.h"

namespace {

constexpr TickType_t START_DELAY_TICKS = pdMS_TO_TICKS(1000);
constexpr TickType_t POLL_INTERVAL_TICKS = pdMS_TO_TICKS(100);
constexpr unsigned long CONNECT_TIMEOUT_MS = 12000UL;

std::atomic<bool> attempted{false};
std::atomic<bool> cancelRequested{false};

[[noreturn]] void finishTask() {
  vTaskDelete(nullptr);
  while (true) {
    vTaskDelay(portMAX_DELAY);
  }
}

void bootClockSyncTask(void*) {
  // Give the first interactive frame a chance to settle and let an explicit
  // Wi-Fi action take ownership before touching the radio.
  vTaskDelay(START_DELAY_TICKS);
  if (cancelRequested.load(std::memory_order_acquire)) finishTask();

  if (TimeUtils::isClockValid()) {
    LOG_DBG("CLK", "Background boot clock sync skipped: clock is already valid");
    finishTask();
  }

  // A foreground Wi-Fi activity may have started during the grace period.
  // Never change its mode or disconnect its connection.
  if (WiFi.getMode() != WIFI_MODE_NULL && WiFi.status() != WL_CONNECTED) {
    LOG_DBG("CLK", "Background boot clock sync skipped: WiFi is in use");
    finishTask();
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const auto credential = WIFI_STORE.findCredential(lastSsid);
  if (lastSsid.empty() || !credential) {
    LOG_DBG("CLK", "Background boot clock sync skipped: no saved WiFi network");
    finishTask();
  }

  bool ownsWifi = false;
  if (WiFi.status() != WL_CONNECTED) {
    // The mode check above ensures this is an idle radio. From this point on,
    // cancellation is checked between connection polls and before NTP.
    ownsWifi = true;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }

  const unsigned long startedAt = millis();
  while (!cancelRequested.load(std::memory_order_acquire) && WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < CONNECT_TIMEOUT_MS) {
    vTaskDelay(POLL_INTERVAL_TICKS);
  }

  if (!cancelRequested.load(std::memory_order_acquire) && WiFi.status() == WL_CONNECTED) {
    LOG_INF("CLK", "Running background boot NTP sync");
    const bool synced = halClock.syncFromNTP();
    LOG_INF("CLK", "Background boot NTP sync %s", synced ? "succeeded" : "failed");
  } else if (!cancelRequested.load(std::memory_order_acquire)) {
    LOG_DBG("CLK", "Background boot clock sync skipped: WiFi connection timed out");
  }

  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }

  finishTask();
}

}  // namespace

namespace BootClockSyncTask {

void start() {
  if (attempted.exchange(true, std::memory_order_acq_rel)) return;

  cancelRequested.store(false, std::memory_order_release);
  const BaseType_t result =
      xTaskCreatePinnedToCore(&bootClockSyncTask, "BootClockSync", 4096, nullptr, 1, nullptr, 0);
  if (result != pdPASS) {
    LOG_DBG("CLK", "Background boot clock sync task could not be created");
  }
}

void cancel() {
  // Cancellation is permanent for this boot. A foreground network activity
  // may call this before the first UI loop gets a chance to create the task.
  cancelRequested.store(true, std::memory_order_release);
  attempted.store(true, std::memory_order_release);
}

}  // namespace BootClockSyncTask
