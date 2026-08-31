#include "AchievementsStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>

#include "util/TimeUtils.h"

namespace {
constexpr char FILE_PATH[] = "/.crosspoint/achievements.json";
constexpr char TMP_PATH[] = "/.crosspoint/achievements.json.tmp";

// Our compact activity groups all book-finished milestones together, while
// crossmux interleaves the first six with the time/goal milestones. Keep an
// explicit compatibility map so an existing crossmux ledger is migrated
// without shifting unrelated unlocks.
size_t crossmuxStateIndex(const size_t index) {
  static constexpr size_t MAP[] = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 43, 12, 13, 44, 14, 45, 15, 46, 47, 53, 48, 54, 49, 55,
      50, 56, 57, 51, 58, 59, 60, 61, 52, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
      32, 37, 38, 39, 40, 41, 42,
  };
  return index < sizeof(MAP) / sizeof(MAP[0]) ? MAP[index] : index;
}

std::vector<AchievementDefinition> makeDefinitions() {
  return {
      {AchievementMetric::BooksStarted, 1, "Start your first book"},
      {AchievementMetric::BooksStarted, 5, "Start 5 books"},
      {AchievementMetric::BooksStarted, 10, "Start 10 books"},
      {AchievementMetric::BooksStarted, 25, "Start 25 books"},
      {AchievementMetric::BooksStarted, 50, "Start 50 books"},
      {AchievementMetric::Sessions, 1, "Complete your first session"},
      {AchievementMetric::Sessions, 10, "Complete 10 sessions"},
      {AchievementMetric::Sessions, 25, "Complete 25 sessions"},
      {AchievementMetric::Sessions, 50, "Complete 50 sessions"},
      {AchievementMetric::Sessions, 100, "Complete 100 sessions"},
      {AchievementMetric::Sessions, 200, "Complete 200 sessions"},
      {AchievementMetric::BooksFinished, 1, "Finish your first book"},
      {AchievementMetric::BooksFinished, 2, "Finish 2 books"},
      {AchievementMetric::BooksFinished, 3, "Finish 3 books"},
      {AchievementMetric::BooksFinished, 5, "Finish 5 books"},
      {AchievementMetric::BooksFinished, 7, "Finish 7 books"},
      {AchievementMetric::BooksFinished, 10, "Finish 10 books"},
      {AchievementMetric::BooksFinished, 15, "Finish 15 books"},
      {AchievementMetric::BooksFinished, 20, "Finish 20 books"},
      {AchievementMetric::BooksFinished, 25, "Finish 25 books"},
      {AchievementMetric::BooksFinished, 30, "Finish 30 books"},
      {AchievementMetric::BooksFinished, 35, "Finish 35 books"},
      {AchievementMetric::BooksFinished, 40, "Finish 40 books"},
      {AchievementMetric::BooksFinished, 45, "Finish 45 books"},
      {AchievementMetric::BooksFinished, 50, "Finish 50 books"},
      {AchievementMetric::BooksFinished, 55, "Finish 55 books"},
      {AchievementMetric::BooksFinished, 60, "Finish 60 books"},
      {AchievementMetric::BooksFinished, 65, "Finish 65 books"},
      {AchievementMetric::BooksFinished, 70, "Finish 70 books"},
      {AchievementMetric::BooksFinished, 75, "Finish 75 books"},
      {AchievementMetric::BooksFinished, 80, "Finish 80 books"},
      {AchievementMetric::BooksFinished, 85, "Finish 85 books"},
      {AchievementMetric::BooksFinished, 90, "Finish 90 books"},
      {AchievementMetric::BooksFinished, 95, "Finish 95 books"},
      {AchievementMetric::BooksFinished, 100, "Finish 100 books"},
      {AchievementMetric::TotalReadingMs, 60ULL * 60ULL * 1000ULL, "Read for 1 hour"},
      {AchievementMetric::TotalReadingMs, 5ULL * 60ULL * 60ULL * 1000ULL, "Read for 5 hours"},
      {AchievementMetric::TotalReadingMs, 10ULL * 60ULL * 60ULL * 1000ULL, "Read for 10 hours"},
      {AchievementMetric::TotalReadingMs, 24ULL * 60ULL * 60ULL * 1000ULL, "Read for 1 day"},
      {AchievementMetric::TotalReadingMs, 50ULL * 60ULL * 60ULL * 1000ULL, "Read for 50 hours"},
      {AchievementMetric::TotalReadingMs, 100ULL * 60ULL * 60ULL * 1000ULL, "Read for 100 hours"},
      {AchievementMetric::TotalReadingMs, 200ULL * 60ULL * 60ULL * 1000ULL, "Read for 200 hours"},
      {AchievementMetric::GoalDays, 1, "Reach your first daily goal"},
      {AchievementMetric::GoalDays, 7, "Reach your goal on 7 days"},
      {AchievementMetric::GoalDays, 30, "Reach your goal on 30 days"},
      {AchievementMetric::GoalDays, 60, "Reach your goal on 60 days"},
      {AchievementMetric::GoalDays, 80, "Reach your goal on 80 days"},
      {AchievementMetric::MaxGoalStreak, 3, "Keep a 3-day goal streak"},
      {AchievementMetric::MaxGoalStreak, 7, "Keep a 7-day goal streak"},
      {AchievementMetric::MaxGoalStreak, 14, "Keep a 14-day goal streak"},
      {AchievementMetric::MaxGoalStreak, 30, "Keep a 30-day goal streak"},
      {AchievementMetric::MaxGoalStreak, 60, "Keep a 60-day goal streak"},
      {AchievementMetric::MaxSessionMs, 15ULL * 60ULL * 1000ULL, "Read for 15 minutes in one session"},
      {AchievementMetric::MaxSessionMs, 30ULL * 60ULL * 1000ULL, "Read for 30 minutes in one session"},
      {AchievementMetric::MaxSessionMs, 45ULL * 60ULL * 1000ULL, "Read for 45 minutes in one session"},
      {AchievementMetric::MaxSessionMs, 60ULL * 60ULL * 1000ULL, "Read for 1 hour in one session"},
      {AchievementMetric::MaxSessionMs, 90ULL * 60ULL * 1000ULL, "Read for 90 minutes in one session"},
      {AchievementMetric::MaxSessionMs, 120ULL * 60ULL * 1000ULL, "Read for 2 hours in one session"},
  };
}

uint64_t metricValue(const ReadingStatsStore& stats, const AchievementMetric metric) {
  switch (metric) {
    case AchievementMetric::BooksStarted: return stats.getBooksStartedCount();
    case AchievementMetric::BooksFinished: return stats.getBooksFinishedCount();
    case AchievementMetric::Sessions: {
      uint64_t total = 0;
      for (const auto& book : stats.getBooks()) total += book.sessions;
      return total;
    }
    case AchievementMetric::TotalReadingMs: return stats.getTotalReadingMs();
    case AchievementMetric::GoalDays: {
      uint64_t total = 0;
      for (const auto& day : stats.getReadingDays()) if (day.readingMs >= getDailyReadingGoalMs()) ++total;
      return total;
    }
    case AchievementMetric::MaxGoalStreak: return stats.getMaxStreakDays();
    case AchievementMetric::MaxSessionMs: return 0;
  }
  return 0;
}
}  // namespace

AchievementsStore AchievementsStore::instance;

void AchievementsStore::ensureLoaded() const {
  if (!loaded) const_cast<AchievementsStore*>(this)->loadFromFile();
}

const std::vector<AchievementDefinition>& AchievementsStore::definitions() {
  static const std::vector<AchievementDefinition> value = makeDefinitions();
  return value;
}

uint32_t AchievementsStore::referenceTimestamp() {
  const uint32_t timestamp = READING_STATS.getDisplayTimestamp();
  return TimeUtils::isClockValid(timestamp) ? timestamp : static_cast<uint32_t>(time(nullptr));
}

uint64_t AchievementsStore::progressFor(const AchievementMetric metric) const {
  return metric == AchievementMetric::MaxSessionMs ? longestSessionMs : metricValue(READING_STATS, metric);
}

bool AchievementsStore::loadFromFile() {
  if (loaded) return true;
  loaded = true;
  states.assign(definitions().size(), AchievementState{});
  longestSessionMs = 0;
  if (!Storage.exists(FILE_PATH) && Storage.exists(TMP_PATH)) Storage.rename(TMP_PATH, FILE_PATH);
  if (!Storage.exists(FILE_PATH)) {
    reconcileFromCurrentStats(false);
    return false;
  }
  const String json = Storage.readFile(FILE_PATH);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("ACH", "Failed to parse achievements");
    return false;
  }
  longestSessionMs = doc["longestSessionMs"] | static_cast<uint32_t>(0);
  const bool isCrossmuxAchievementFile = !doc["accumulatedReadingMs"].isNull() || !doc["goalDaysCount"].isNull();
  std::vector<AchievementState> persistedStates;
  for (JsonObject obj : doc["states"].as<JsonArray>()) {
    persistedStates.push_back({obj["unlocked"] | false, obj["unlockedAt"] | static_cast<uint32_t>(0)});
  }
  for (size_t index = 0; index < states.size(); ++index) {
    const size_t sourceIndex = isCrossmuxAchievementFile ? crossmuxStateIndex(index) : index;
    if (sourceIndex >= persistedStates.size()) break;
    states[index] = persistedStates[sourceIndex];
  }
  dirty = false;
  reconcileFromCurrentStats(false);
  return true;
}

bool AchievementsStore::saveToFile() const {
  if (!loaded && !dirty) return true;
  if (!dirty && Storage.exists(FILE_PATH)) return true;
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["formatVersion"] = 2;
  JsonArray array = doc["states"].to<JsonArray>();
  for (const auto& state : states) {
    JsonObject obj = array.add<JsonObject>();
    obj["unlocked"] = state.unlocked;
    obj["unlockedAt"] = state.unlockedAt;
  }
  doc["longestSessionMs"] = longestSessionMs;
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(TMP_PATH, json)) return false;
  if (Storage.exists(FILE_PATH)) Storage.remove(FILE_PATH);
  const bool saved = Storage.rename(TMP_PATH, FILE_PATH);
  if (saved) dirty = false;
  return saved;
}

void AchievementsStore::reconcileFromCurrentStats(const bool persist, const bool enqueuePopups) {
  ensureLoaded();
  if (states.size() != definitions().size()) states.assign(definitions().size(), AchievementState{});
  if (!SETTINGS.achievementsEnabled) return;
  const uint32_t previousLongestSessionMs = longestSessionMs;
  for (const auto& book : READING_STATS.getBooks()) longestSessionMs = std::max(longestSessionMs, book.lastSessionMs);
  if (longestSessionMs != previousLongestSessionMs) dirty = true;
  const uint32_t timestamp = referenceTimestamp();
  for (size_t i = 0; i < definitions().size(); ++i) {
    if (!states[i].unlocked && progressFor(definitions()[i].metric) >= definitions()[i].target) {
      states[i].unlocked = true;
      states[i].unlockedAt = timestamp;
      dirty = true;
      if (enqueuePopups) pendingUnlocks.push_back(i);
    }
  }
  if (persist && dirty) saveToFile();
}

void AchievementsStore::recordSessionEnded(const ReadingSessionSnapshot& snapshot) {
  if (!snapshot.valid || !SETTINGS.achievementsEnabled) return;
  ensureLoaded();
  if (snapshot.counted && snapshot.sessionMs > longestSessionMs) {
    longestSessionMs = snapshot.sessionMs;
    dirty = true;
  }
  reconcileFromCurrentStats(true, true);
}

void AchievementsStore::reset() {
  states.assign(definitions().size(), AchievementState{});
  pendingUnlocks.clear();
  longestSessionMs = 0;
  loaded = true;
  dirty = true;
  saveToFile();
}

void AchievementsStore::clear() {
  states.assign(definitions().size(), AchievementState{});
  pendingUnlocks.clear();
  longestSessionMs = 0;
  loaded = true;
  dirty = false;
  if (Storage.exists(FILE_PATH) && !Storage.remove(FILE_PATH)) LOG_ERR("ACH", "Failed to remove achievements");
  if (Storage.exists(TMP_PATH) && !Storage.remove(TMP_PATH)) LOG_ERR("ACH", "Failed to remove achievements temp file");
}

std::vector<AchievementView> AchievementsStore::buildViews() const {
  ensureLoaded();
  std::vector<AchievementView> result;
  result.reserve(definitions().size());
  for (size_t i = 0; i < definitions().size(); ++i)
    result.push_back({&definitions()[i], states.size() > i ? states[i] : AchievementState{}, progressFor(definitions()[i].metric)});
  std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.state.unlocked != right.state.unlocked ? left.state.unlocked > right.state.unlocked : false;
  });
  return result;
}

size_t AchievementsStore::unlockedCount() const {
  ensureLoaded();
  return static_cast<size_t>(std::count_if(states.begin(), states.end(), [](const auto& state) { return state.unlocked; }));
}

std::string AchievementsStore::popNextPopupMessage() {
  if (pendingUnlocks.empty()) return {};
  const size_t index = pendingUnlocks.front();
  pendingUnlocks.erase(pendingUnlocks.begin());
  if (index >= definitions().size()) return {};
  return std::string("Achievement unlocked: ") + definitions()[index].title;
}
