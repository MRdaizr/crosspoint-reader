#include "FlashcardStatsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <utility>

#include "util/TimeUtils.h"

namespace {
constexpr char STATS_FILE_JSON[] = "/.crosspoint/flashcard_stats.json";
constexpr size_t MAX_DAILY_ENTRIES = 366;

std::string currentDate() { return TimeUtils::formatDate(TimeUtils::getCurrentValidTimestamp()); }
}  // namespace

FlashcardStatsStore FlashcardStatsStore::instance;

void FlashcardStatsStore::sortAndTrim() {
  std::sort(dailyEntries.begin(), dailyEntries.end(),
            [](const auto& left, const auto& right) { return left.date > right.date; });
  if (dailyEntries.size() > MAX_DAILY_ENTRIES) dailyEntries.resize(MAX_DAILY_ENTRIES);
}

FlashcardDailyStats* FlashcardStatsStore::todayEntry() {
  const std::string date = currentDate();
  if (date.empty()) return nullptr;
  auto it = std::find_if(dailyEntries.begin(), dailyEntries.end(), [&date](const auto& entry) {
    return entry.date == date;
  });
  if (it == dailyEntries.end()) {
    dailyEntries.push_back({date});
    sortAndTrim();
    it = std::find_if(dailyEntries.begin(), dailyEntries.end(), [&date](const auto& entry) {
      return entry.date == date;
    });
  }
  return it == dailyEntries.end() ? nullptr : &*it;
}

void FlashcardStatsStore::loadFromFile() {
  if (loaded) return;
  loaded = true;
  dailyEntries.clear();

  if (!Storage.exists(STATS_FILE_JSON)) return;
  const String json = Storage.readFile(STATS_FILE_JSON);
  if (json.isEmpty()) return;

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("FCS", "JSON parse error: %s", error.c_str());
    return;
  }

  for (JsonObject item : doc["daily"].as<JsonArray>()) {
    FlashcardDailyStats entry;
    entry.date = item["date"] | std::string("");
    entry.completed = item["completed"] | 0;
    entry.newCards = item["newCards"] | 0;
    entry.learningReviews = item["learningReviews"] | 0;
    entry.reviewReviews = item["reviewReviews"] | 0;
    entry.again = item["again"] | 0;
    entry.hard = item["hard"] | 0;
    entry.good = item["good"] | 0;
    if (!entry.date.empty()) dailyEntries.push_back(std::move(entry));
  }
  sortAndTrim();
}

bool FlashcardStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  JsonArray daily = doc["daily"].to<JsonArray>();
  for (const auto& entry : dailyEntries) {
    JsonObject item = daily.add<JsonObject>();
    item["date"] = entry.date;
    item["completed"] = entry.completed;
    item["newCards"] = entry.newCards;
    item["learningReviews"] = entry.learningReviews;
    item["reviewReviews"] = entry.reviewReviews;
    item["again"] = entry.again;
    item["hard"] = entry.hard;
    item["good"] = entry.good;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(STATS_FILE_JSON, json);
}

void FlashcardStatsStore::clear() {
  dailyEntries.clear();
  loaded = true;
  if (Storage.exists(STATS_FILE_JSON) && !Storage.remove(STATS_FILE_JSON)) {
    LOG_ERR("FCS", "Failed to remove flashcard stats");
  }
}

void FlashcardStatsStore::recordNewCard() {
  loadFromFile();
  auto* entry = todayEntry();
  if (!entry) return;
  ++entry->newCards;
  if (!saveToFile()) LOG_ERR("FCS", "Failed to save flashcard stats");
}

void FlashcardStatsStore::recordReview(const FlashcardGrade grade, const bool reviewCard) {
  loadFromFile();
  auto* entry = todayEntry();
  if (!entry) return;
  ++entry->completed;
  if (reviewCard) ++entry->reviewReviews;
  else ++entry->learningReviews;
  switch (grade) {
    case FlashcardGrade::AGAIN: ++entry->again; break;
    case FlashcardGrade::HARD: ++entry->hard; break;
    case FlashcardGrade::GOOD: ++entry->good; break;
  }
  if (!saveToFile()) LOG_ERR("FCS", "Failed to save flashcard stats");
}

const std::vector<FlashcardDailyStats>& FlashcardStatsStore::getRecentDailyEntries() {
  loadFromFile();
  sortAndTrim();
  return dailyEntries;
}
