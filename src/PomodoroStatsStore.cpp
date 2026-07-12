#include "PomodoroStatsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
constexpr char STATS_FILE_JSON[] = "/.crosspoint/pomodoro_stats.json";
constexpr time_t VALID_EPOCH = 1700000000;  // 2023-11-14
constexpr size_t MAX_DAILY_ENTRIES = 7;

bool currentDate(char (&date)[11]) {
  const time_t now = time(nullptr);
  if (now < VALID_EPOCH) return false;

  struct tm localTime {};
  localtime_r(&now, &localTime);
  snprintf(date, sizeof(date), "%04d-%02d-%02d", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday);
  return true;
}

void sortNewestFirst(std::vector<PomodoroDailyEntry>& entries) {
  std::sort(entries.begin(), entries.end(),
            [](const PomodoroDailyEntry& left, const PomodoroDailyEntry& right) { return left.date > right.date; });
}

void discardExpiredEntries(std::vector<PomodoroDailyEntry>& entries) {
  const time_t now = time(nullptr);
  if (now < VALID_EPOCH) return;

  const time_t earliest = now - static_cast<time_t>(MAX_DAILY_ENTRIES - 1) * 24 * 60 * 60;
  struct tm localTime {};
  localtime_r(&earliest, &localTime);
  char earliestDate[11];
  snprintf(earliestDate, sizeof(earliestDate), "%04d-%02d-%02d", localTime.tm_year + 1900, localTime.tm_mon + 1,
           localTime.tm_mday);
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&earliestDate](const PomodoroDailyEntry& entry) {
                  return entry.date < earliestDate;
                }),
                entries.end());
}

}  // namespace

PomodoroStatsStore PomodoroStatsStore::instance;

void PomodoroStatsStore::loadFromFile() {
  if (loaded) return;
  loaded = true;
  totalCompletedFocuses = 0;
  totalFocusSeconds = 0;
  dailyEntries.clear();

  if (!Storage.exists(STATS_FILE_JSON)) return;
  const String json = Storage.readFile(STATS_FILE_JSON);
  if (json.isEmpty()) return;

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PST", "JSON parse error: %s", error.c_str());
    return;
  }

  totalCompletedFocuses = doc["totalCompletedFocuses"] | 0;
  totalFocusSeconds = doc["totalFocusSeconds"] | 0;
  for (JsonObject entry : doc["daily"].as<JsonArray>()) {
    PomodoroDailyEntry daily;
    daily.date = entry["date"] | std::string("");
    daily.completedFocuses = entry["completedFocuses"] | 0;
    daily.focusSeconds = entry["focusSeconds"] | 0;
    if (!daily.date.empty()) dailyEntries.push_back(std::move(daily));
  }
  sortNewestFirst(dailyEntries);
  discardExpiredEntries(dailyEntries);
  if (dailyEntries.size() > MAX_DAILY_ENTRIES) dailyEntries.resize(MAX_DAILY_ENTRIES);
}

bool PomodoroStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["totalCompletedFocuses"] = totalCompletedFocuses;
  doc["totalFocusSeconds"] = totalFocusSeconds;
  JsonArray daily = doc["daily"].to<JsonArray>();
  for (const auto& entry : dailyEntries) {
    JsonObject item = daily.add<JsonObject>();
    item["date"] = entry.date;
    item["completedFocuses"] = entry.completedFocuses;
    item["focusSeconds"] = entry.focusSeconds;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(STATS_FILE_JSON, json);
}

void PomodoroStatsStore::clear() {
  totalCompletedFocuses = 0;
  totalFocusSeconds = 0;
  dailyEntries.clear();
  loaded = true;
  if (Storage.exists(STATS_FILE_JSON) && !Storage.remove(STATS_FILE_JSON)) {
    LOG_ERR("PST", "Failed to remove pomodoro stats");
  }
}

void PomodoroStatsStore::recordCompletedFocus(const uint32_t seconds) {
  loadFromFile();
  totalCompletedFocuses++;
  totalFocusSeconds += seconds;

  char today[11] = {};
  if (currentDate(today)) {
    discardExpiredEntries(dailyEntries);
    auto it = std::find_if(dailyEntries.begin(), dailyEntries.end(), [&today](const PomodoroDailyEntry& entry) {
      return entry.date == today;
    });
    if (it == dailyEntries.end()) {
      dailyEntries.push_back({today, 1, seconds});
    } else {
      it->completedFocuses++;
      it->focusSeconds += seconds;
    }
    sortNewestFirst(dailyEntries);
    if (dailyEntries.size() > MAX_DAILY_ENTRIES) dailyEntries.resize(MAX_DAILY_ENTRIES);
  }

  if (!saveToFile()) LOG_ERR("PST", "Failed to save pomodoro stats");
}

bool PomodoroStatsStore::hasValidDate() const {
  char today[11] = {};
  return currentDate(today);
}

uint32_t PomodoroStatsStore::getTotalCompletedFocuses() {
  loadFromFile();
  return totalCompletedFocuses;
}

uint32_t PomodoroStatsStore::getTotalFocusSeconds() {
  loadFromFile();
  return totalFocusSeconds;
}

const std::vector<PomodoroDailyEntry>& PomodoroStatsStore::getRecentDailyEntries() {
  loadFromFile();
  discardExpiredEntries(dailyEntries);
  return dailyEntries;
}
