#include "ReadingStatsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace {
constexpr char STATS_FILE_JSON[] = "/.crosspoint/reading_stats.json";
constexpr int MAX_STATS_ENTRIES = 50;

std::string fallbackTitle(const std::string& path, const std::string& title) {
  if (!title.empty()) return title;
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void sortByReadingTime(std::vector<ReadingStatEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const ReadingStatEntry& left, const ReadingStatEntry& right) {
    if (left.totalSeconds != right.totalSeconds) return left.totalSeconds > right.totalSeconds;
    return left.title < right.title;
  });
}
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::loadFromFile() {
  if (loaded) return;
  loaded = true;
  entries.clear();

  if (!Storage.exists(STATS_FILE_JSON)) return;
  const String json = Storage.readFile(STATS_FILE_JSON);
  if (json.isEmpty()) return;

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RST", "JSON parse error: %s", error.c_str());
    return;
  }

  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (static_cast<int>(entries.size()) >= MAX_STATS_ENTRIES) break;
    ReadingStatEntry entry;
    entry.path = obj["path"] | std::string("");
    entry.title = obj["title"] | std::string("");
    entry.totalSeconds = obj["totalSeconds"] | 0;
    if (!entry.path.empty()) entries.push_back(entry);
  }
  sortByReadingTime(entries);
}

bool ReadingStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = entry.path;
    obj["title"] = entry.title;
    obj["totalSeconds"] = entry.totalSeconds;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(STATS_FILE_JSON, json);
}

void ReadingStatsStore::clear() {
  entries.clear();
  loaded = true;
  if (Storage.exists(STATS_FILE_JSON) && !Storage.remove(STATS_FILE_JSON)) {
    LOG_ERR("RST", "Failed to remove reading stats");
  }
}

void ReadingStatsStore::addSession(const std::string& path, const std::string& title, uint32_t seconds) {
  if (path.empty() || seconds == 0) return;
  loadFromFile();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const ReadingStatEntry& entry) {
    return entry.path == path;
  });
  if (it == entries.end()) {
    entries.push_back({path, fallbackTitle(path, title), seconds});
  } else {
    it->title = fallbackTitle(path, title);
    it->totalSeconds += seconds;
  }
  sortByReadingTime(entries);
  if (static_cast<int>(entries.size()) > MAX_STATS_ENTRIES) entries.resize(MAX_STATS_ENTRIES);

  if (!saveToFile()) {
    LOG_ERR("RST", "Failed to save reading stats");
  }
}

const std::vector<ReadingStatEntry>& ReadingStatsStore::getEntries() {
  loadFromFile();
  return entries;
}
