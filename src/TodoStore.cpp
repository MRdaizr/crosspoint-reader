#include "TodoStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <limits>

namespace {
constexpr const char* TODO_PATH = "/.crosspoint/todos.json";
constexpr const char* TODO_TMP_PATH = "/.crosspoint/todos.json.tmp";
constexpr const char* TODO_BACKUP_PATH = "/.crosspoint/todos.json.bak";
constexpr uint8_t TODO_VERSION = 1;

bool loadFromPath(const char* path, std::vector<TodoItem>& items, uint32_t& nextId) {
  if (!Storage.exists(path)) return false;

  const String json = Storage.readFile(path);
  if (json.isEmpty()) return false;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);
  if (error || (doc["version"] | 0) != TODO_VERSION) return false;

  const JsonArray source = doc["items"].as<JsonArray>();
  uint32_t maxId = 0;
  for (JsonObject entry : source) {
    if (items.size() >= TodoStore::MAX_ITEMS) break;
    TodoItem item;
    item.id = entry["id"] | 0U;
    item.title = entry["title"] | std::string("");
    item.scheduledAt = entry["scheduledAt"] | std::string("");
    item.completed = entry["completed"] | false;
    if (item.id == 0 || item.title.empty() || item.title.size() > TodoStore::MAX_TITLE_BYTES ||
        (!item.scheduledAt.empty() && !TodoStore::isValidScheduledAt(item.scheduledAt))) {
      continue;
    }
    maxId = std::max(maxId, item.id);
    items.push_back(std::move(item));
  }

  nextId = doc["nextId"] | (maxId + 1U);
  if (nextId == 0 || nextId <= maxId) nextId = maxId == std::numeric_limits<uint32_t>::max() ? 0 : maxId + 1U;
  return true;
}

std::string trimTitle(const std::string& value) {
  size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
  size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
  return value.substr(first, last - first);
}
}  // namespace

TodoStore TodoStore::instance;

bool TodoStore::isValidScheduledAt(const std::string& value) {
  if (value.size() != 16 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':') return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13) continue;
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  const auto numberAt = [&value](size_t offset) { return (value[offset] - '0') * 10 + (value[offset + 1] - '0'); };
  const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0');
  const int month = numberAt(5);
  const int day = numberAt(8);
  const int hour = numberAt(11);
  const int minute = numberAt(14);
  if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59) return false;
  static constexpr uint8_t DAYS_IN_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int maxDay = month == 2 && leapYear ? 29 : DAYS_IN_MONTH[month - 1];
  return day >= 1 && day <= maxDay;
}

void TodoStore::sortItems(std::vector<TodoItem>& items) {
  std::sort(items.begin(), items.end(), [](const TodoItem& left, const TodoItem& right) {
    if (left.completed != right.completed) return !left.completed;
    if (left.scheduledAt.empty() != right.scheduledAt.empty()) return !left.scheduledAt.empty();
    if (left.scheduledAt != right.scheduledAt) return left.scheduledAt < right.scheduledAt;
    return left.id < right.id;
  });
}

bool TodoStore::load(std::vector<TodoItem>& items, uint32_t& nextId) const {
  items.clear();
  nextId = 1;

  if (!Storage.exists(TODO_PATH)) {
    return !Storage.exists(TODO_BACKUP_PATH) || loadFromPath(TODO_BACKUP_PATH, items, nextId);
  }
  if (!loadFromPath(TODO_PATH, items, nextId)) {
    items.clear();
    if (!loadFromPath(TODO_BACKUP_PATH, items, nextId)) {
      LOG_ERR("TODO", "Failed to parse todo list");
      return false;
    }
    LOG_DBG("TODO", "Recovered todo list from backup");
  }
  sortItems(items);
  return true;
}

bool TodoStore::save(const std::vector<TodoItem>& items, uint32_t nextId) const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["version"] = TODO_VERSION;
  doc["nextId"] = nextId;
  JsonArray destination = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject entry = destination.add<JsonObject>();
    entry["id"] = item.id;
    entry["title"] = item.title;
    if (!item.scheduledAt.empty()) entry["scheduledAt"] = item.scheduledAt;
    entry["completed"] = item.completed;
  }

  String json;
  serializeJson(doc, json);
  Storage.remove(TODO_TMP_PATH);
  if (!Storage.writeFile(TODO_TMP_PATH, json)) return false;

  Storage.remove(TODO_BACKUP_PATH);
  const bool hadCurrent = Storage.exists(TODO_PATH);
  if (hadCurrent && !Storage.rename(TODO_PATH, TODO_BACKUP_PATH)) {
    Storage.remove(TODO_TMP_PATH);
    return false;
  }
  if (!Storage.rename(TODO_TMP_PATH, TODO_PATH)) {
    if (hadCurrent) Storage.rename(TODO_BACKUP_PATH, TODO_PATH);
    return false;
  }
  Storage.remove(TODO_BACKUP_PATH);
  return true;
}

bool TodoStore::getItems(std::vector<TodoItem>& items) const {
  uint32_t nextId = 1;
  return load(items, nextId);
}

bool TodoStore::add(const std::string& rawTitle, const std::string& scheduledAt, TodoItem& item) {
  const std::string title = trimTitle(rawTitle);
  if (title.empty() || title.size() > MAX_TITLE_BYTES || !TodoStore::isValidScheduledAt(scheduledAt)) return false;

  std::vector<TodoItem> items;
  uint32_t nextId = 1;
  if (!load(items, nextId) || items.size() >= MAX_ITEMS || nextId == 0) return false;

  item = {nextId, title, scheduledAt, false};
  items.push_back(item);
  ++nextId;
  sortItems(items);
  return save(items, nextId);
}

bool TodoStore::updateScheduledAt(const uint32_t id, const std::string& scheduledAt, TodoItem& item) {
  if (id == 0 || !TodoStore::isValidScheduledAt(scheduledAt)) return false;

  std::vector<TodoItem> items;
  uint32_t nextId = 1;
  if (!load(items, nextId)) return false;

  const auto found = std::find_if(items.begin(), items.end(), [id](const TodoItem& candidate) {
    return candidate.id == id;
  });
  if (found == items.end()) return false;

  found->scheduledAt = scheduledAt;
  item = *found;
  sortItems(items);
  return save(items, nextId);
}

bool TodoStore::toggle(uint32_t id, TodoItem& item) {
  std::vector<TodoItem> items;
  uint32_t nextId = 1;
  if (!load(items, nextId)) return false;

  const auto found = std::find_if(items.begin(), items.end(), [id](const TodoItem& candidate) { return candidate.id == id; });
  if (found == items.end()) return false;
  found->completed = !found->completed;
  item = *found;
  sortItems(items);
  return save(items, nextId);
}

bool TodoStore::remove(uint32_t id) {
  std::vector<TodoItem> items;
  uint32_t nextId = 1;
  if (!load(items, nextId)) return false;

  const auto found = std::remove_if(items.begin(), items.end(), [id](const TodoItem& candidate) { return candidate.id == id; });
  if (found == items.end()) return false;
  items.erase(found, items.end());
  return save(items, nextId);
}
