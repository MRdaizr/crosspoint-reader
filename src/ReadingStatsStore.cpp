#include "ReadingStatsStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <utility>

#include "util/BookIdentity.h"
#include "util/TimeUtils.h"

namespace {
constexpr char STATS_FILE[] = "/.crosspoint/reading_stats.json";
constexpr char STATS_TMP_FILE[] = "/.crosspoint/reading_stats.json.tmp";
constexpr unsigned long MAX_READING_GAP_MS = 30UL * 60UL * 1000UL;
constexpr unsigned long HEARTBEAT_MS = 60UL * 1000UL;
constexpr unsigned long CHECKPOINT_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long RETRY_MS = 30UL * 1000UL;
constexpr uint64_t MIN_SESSION_MS = 3ULL * 60ULL * 1000ULL;
constexpr size_t MAX_SESSION_LOG = 256;

void addDay(std::vector<ReadingDayStats>& days, uint32_t ordinal, uint64_t milliseconds) {
  if (!ordinal || !milliseconds) return;
  auto it = std::lower_bound(days.begin(), days.end(), ordinal,
                             [](const ReadingDayStats& day, uint32_t value) { return day.dayOrdinal < value; });
  if (it == days.end() || it->dayOrdinal != ordinal) days.insert(it, {ordinal, milliseconds});
  else it->readingMs += milliseconds;
}

void appendDays(JsonArray array, const std::vector<ReadingDayStats>& days) {
  for (const auto& day : days) {
    JsonObject obj = array.add<JsonObject>();
    obj["dayOrdinal"] = day.dayOrdinal;
    obj["readingMs"] = day.readingMs;
  }
}

void parseDays(JsonArray source, std::vector<ReadingDayStats>& destination) {
  for (JsonVariant value : source) {
    ReadingDayStats day;
    if (value.is<JsonObject>()) {
      JsonObject obj = value.as<JsonObject>();
      day.dayOrdinal = obj["dayOrdinal"] | static_cast<uint32_t>(0);
      day.readingMs = obj["readingMs"] | static_cast<uint64_t>(0);
      if (!day.readingMs) day.readingMs = (obj["readingSeconds"] | static_cast<uint64_t>(0)) * 1000ULL;
    } else {
      day.dayOrdinal = value | static_cast<uint32_t>(0);
    }
    if (day.dayOrdinal) destination.push_back(day);
  }
}

bool writeDocument(const char* path, JsonDocument& document) {
  String json;
  serializeJson(document, json);
  return Storage.writeFile(path, json);
}
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

bool ReadingStatsStore::isClockValid(const uint32_t timestamp) { return TimeUtils::isClockValid(timestamp); }

bool ReadingStatsStore::shouldIgnorePath(const std::string& path) {
  std::string normalized = BookIdentity::normalizePath(path);
  if (normalized.empty()) return false;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return normalized == "/ignore_stats" || normalized.rfind("/ignore_stats/", 0) == 0 ||
         normalized == "/if_found.txt" || normalized == "/if_found.txt.txt";
}

size_t ReadingStatsStore::findBookIndexByPath(const std::string& path) const {
  const std::string normalized = BookIdentity::normalizePath(path);
  if (normalized.empty()) return books.size();
  for (size_t i = 0; i < books.size(); ++i) {
    const auto& known = books[i].knownPaths;
    if (books[i].path == normalized || std::find(known.begin(), known.end(), normalized) != known.end()) return i;
  }
  return books.size();
}

size_t ReadingStatsStore::findBookIndexById(const std::string& id) const {
  if (id.empty()) return books.size();
  for (size_t i = 0; i < books.size(); ++i) if (books[i].bookId == id) return i;
  return books.size();
}

size_t ReadingStatsStore::createOrFindBook(const std::string& path, const std::string& title,
                                           const std::string& author, const std::string& coverBmpPath) {
  const std::string normalized = BookIdentity::normalizePath(path);
  const std::string id = BookIdentity::resolveStableBookId(normalized);
  size_t index = findBookIndexByPath(normalized);
  if (index == books.size()) index = findBookIndexById(id);
  if (index == books.size()) {
    ReadingBookStats book;
    book.bookId = id;
    book.path = normalized;
    if (!normalized.empty()) book.knownPaths.push_back(normalized);
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    books.insert(books.begin(), std::move(book));
    return 0;
  }
  auto& book = books[index];
  if (!normalized.empty() && std::find(book.knownPaths.begin(), book.knownPaths.end(), normalized) == book.knownPaths.end())
    book.knownPaths.push_back(normalized);
  if (!normalized.empty()) book.path = normalized;
  if (book.bookId.empty() || (BookIdentity::isLegacyBookId(book.bookId) && !BookIdentity::isLegacyBookId(id))) book.bookId = id;
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  if (!coverBmpPath.empty()) book.coverBmpPath = coverBmpPath;
  return index;
}

void ReadingStatsStore::normalizeDays(std::vector<ReadingDayStats>& days) {
  std::sort(days.begin(), days.end(), [](const auto& left, const auto& right) { return left.dayOrdinal < right.dayOrdinal; });
  std::vector<ReadingDayStats> merged;
  for (const auto& day : days) addDay(merged, day.dayOrdinal, day.readingMs);
  days = std::move(merged);
}

void ReadingStatsStore::normalize() {
  books.erase(std::remove_if(books.begin(), books.end(), [](const ReadingBookStats& book) {
                return shouldIgnorePath(book.path);
              }),
              books.end());
  for (auto& book : books) {
    book.path = BookIdentity::normalizePath(book.path);
    if (book.bookId.empty()) book.bookId = BookIdentity::resolveStableBookId(book.path);
    for (auto& path : book.knownPaths) path = BookIdentity::normalizePath(path);
    if (!book.path.empty() && std::find(book.knownPaths.begin(), book.knownPaths.end(), book.path) == book.knownPaths.end())
      book.knownPaths.push_back(book.path);
    std::sort(book.knownPaths.begin(), book.knownPaths.end());
    book.knownPaths.erase(std::unique(book.knownPaths.begin(), book.knownPaths.end()), book.knownPaths.end());
    normalizeDays(book.readingDays);
    book.lastProgressPercent = std::min<uint8_t>(book.lastProgressPercent, 100);
    book.chapterProgressPercent = std::min<uint8_t>(book.chapterProgressPercent, 100);
  }
  for (size_t i = 0; i < books.size(); ++i) {
    for (size_t j = i + 1; j < books.size();) {
      if (books[i].bookId.empty() || books[i].bookId != books[j].bookId) { ++j; continue; }
      auto& first = books[i];
      auto& duplicate = books[j];
      first.totalReadingMs += duplicate.totalReadingMs;
      first.sessions += duplicate.sessions;
      first.lastSessionMs = std::max(first.lastSessionMs, duplicate.lastSessionMs);
      first.firstReadAt = first.firstReadAt == 0 ? duplicate.firstReadAt :
                          (duplicate.firstReadAt == 0 ? first.firstReadAt : std::min(first.firstReadAt, duplicate.firstReadAt));
      first.lastReadAt = std::max(first.lastReadAt, duplicate.lastReadAt);
      first.completedAt = std::max(first.completedAt, duplicate.completedAt);
      first.completed = first.completed || duplicate.completed;
      first.lastProgressPercent = std::max(first.lastProgressPercent, duplicate.lastProgressPercent);
      first.readingDays.insert(first.readingDays.end(), duplicate.readingDays.begin(), duplicate.readingDays.end());
      for (const auto& path : duplicate.knownPaths)
        if (std::find(first.knownPaths.begin(), first.knownPaths.end(), path) == first.knownPaths.end()) first.knownPaths.push_back(path);
      normalizeDays(first.readingDays);
      books.erase(books.begin() + static_cast<std::ptrdiff_t>(j));
    }
  }
}

void ReadingStatsStore::rebuildAggregatedDays() {
  readingDays = legacyReadingDays;
  normalizeDays(readingDays);
  for (const auto& book : books) for (const auto& day : book.readingDays) addDay(readingDays, day.dayOrdinal, day.readingMs);
}

void ReadingStatsStore::markDirty() {
  if (!dirty) dirtySinceMs = millis();
  dirty = true;
}

uint32_t ReadingStatsStore::latestKnownTimestamp() const {
  uint32_t latest = 0;
  for (const auto& book : books)
    latest = std::max(latest, std::max(book.lastReadAt, std::max(book.firstReadAt, book.completedAt)));
  return latest;
}

uint32_t ReadingStatsStore::referenceTimestamp(const uint32_t preferred, const uint32_t bookTimestamp) const {
  if (isClockValid(preferred)) return preferred;
  const uint32_t current = TimeUtils::getAuthoritativeTimestamp();
  if (isClockValid(current)) return current;
  const uint32_t latest = latestKnownTimestamp();
  if (isClockValid(latest)) return latest;
  return isClockValid(bookTimestamp) ? bookTimestamp : 0;
}

void ReadingStatsStore::recordReadingTime(ReadingBookStats& book, const uint32_t timestamp,
                                          const uint64_t milliseconds) {
  if (!isClockValid(timestamp) || !milliseconds) return;
  const uint32_t ordinal = TimeUtils::getLocalDayOrdinal(timestamp);
  addDay(book.readingDays, ordinal, milliseconds);
  addDay(readingDays, ordinal, milliseconds);
}

void ReadingStatsStore::appendSessionLog(const uint32_t dayOrdinal, const uint32_t sessionMs) {
  if (!dayOrdinal || !sessionMs) return;
  sessionLog.push_back({dayOrdinal, sessionMs});
  if (sessionLog.size() > MAX_SESSION_LOG) sessionLog.erase(sessionLog.begin(), sessionLog.begin() + (sessionLog.size() - MAX_SESSION_LOG));
}

void ReadingStatsStore::creditElapsed() {
  if (!activeSession.active || activeSession.bookIndex >= books.size()) return;
  const unsigned long now = millis();
  const unsigned long elapsed = std::min(now - activeSession.lastActivityMs, MAX_READING_GAP_MS);
  if (!elapsed) return;
  auto& book = books[activeSession.bookIndex];
  const uint32_t timestamp = referenceTimestamp(TimeUtils::getAuthoritativeTimestamp(), book.lastReadAt);
  book.totalReadingMs += elapsed;
  activeSession.accumulatedMs += elapsed;
  recordReadingTime(book, timestamp, elapsed);
  if (isClockValid(timestamp)) {
    if (!book.firstReadAt) book.firstReadAt = timestamp;
    book.lastReadAt = timestamp;
  }
  activeSession.lastActivityMs = now;
  markDirty();
}

void ReadingStatsStore::beginSession(const std::string& path, const std::string& title, const std::string& author,
                                     const std::string& coverBmpPath, const uint8_t progressPercent,
                                     const std::string& chapterTitle, const uint8_t chapterProgressPercent) {
  loadFromFile();
  if (activeSession.active) endSession();
  if (path.empty() || shouldIgnorePath(path)) return;
  const size_t index = createOrFindBook(path, title, author, coverBmpPath);
  auto& book = books[index];
  activeSession = {true, index, millis(), 0, book.lastProgressPercent, book.completed};
  book.lastProgressPercent = std::min<uint8_t>(progressPercent, 100);
  book.chapterTitle = chapterTitle;
  book.chapterProgressPercent = std::min<uint8_t>(chapterProgressPercent, 100);
  if (book.lastProgressPercent >= 100) book.completed = true;
  const uint32_t timestamp = referenceTimestamp(TimeUtils::getAuthoritativeTimestamp(), book.lastReadAt);
  if (isClockValid(timestamp)) {
    if (!book.firstReadAt) book.firstReadAt = timestamp;
    book.lastReadAt = timestamp;
  }
  markDirty();
}

void ReadingStatsStore::noteActivity() {
  if (!activeSession.active) return;
  creditElapsed();
  activeSession.lastActivityMs = millis();
}

void ReadingStatsStore::tickActiveSession() {
  if (activeSession.active && millis() - activeSession.lastActivityMs >= HEARTBEAT_MS) noteActivity();
}

void ReadingStatsStore::resumeSession() {
  if (activeSession.active) activeSession.lastActivityMs = millis();
}

void ReadingStatsStore::updateProgress(const uint8_t progressPercent, const bool completed,
                                       const std::string& chapterTitle, const uint8_t chapterProgressPercent) {
  if (!activeSession.active || activeSession.bookIndex >= books.size()) return;
  auto& book = books[activeSession.bookIndex];
  const uint8_t progress = std::min<uint8_t>(progressPercent, 100);
  const uint8_t chapterProgress = std::min<uint8_t>(chapterProgressPercent, 100);
  if (book.lastProgressPercent == progress && book.chapterTitle == chapterTitle &&
      book.chapterProgressPercent == chapterProgress && (!completed || book.completed)) return;
  book.lastProgressPercent = progress;
  book.chapterTitle = chapterTitle;
  book.chapterProgressPercent = chapterProgress;
  if (completed || progress >= 100) {
    book.completed = true;
    if (!book.completedAt) book.completedAt = referenceTimestamp(TimeUtils::getAuthoritativeTimestamp(), book.lastReadAt);
  }
  markDirty();
}

void ReadingStatsStore::endSession() {
  if (!activeSession.active || activeSession.bookIndex >= books.size()) { activeSession = {}; lastSessionSnapshot = {}; return; }
  creditElapsed();
  auto& book = books[activeSession.bookIndex];
  const uint32_t sessionMs = static_cast<uint32_t>(std::min<uint64_t>(activeSession.accumulatedMs, UINT32_MAX));
  const bool counted = activeSession.accumulatedMs >= MIN_SESSION_MS;
  if (counted) {
    ++book.sessions;
    book.lastSessionMs = sessionMs;
    const uint32_t timestamp = referenceTimestamp(TimeUtils::getAuthoritativeTimestamp(), book.lastReadAt);
    if (isClockValid(timestamp)) appendSessionLog(TimeUtils::getLocalDayOrdinal(timestamp), sessionMs);
    markDirty();
  }
  lastSessionSnapshot = {true, ++sessionSerial, book.bookId, book.path, sessionMs, counted,
                         !activeSession.startCompleted && book.completed, activeSession.startProgressPercent,
                         book.lastProgressPercent};
  activeSession = {};
  if (dirty) saveToFile();
}

void ReadingStatsStore::addSession(const std::string& path, const std::string& title, const uint32_t seconds) {
  if (!seconds) return;
  beginSession(path, title);
  if (!activeSession.active) return;
  activeSession.accumulatedMs = static_cast<uint64_t>(seconds) * 1000ULL;
  activeSession.lastActivityMs = millis();
  endSession();
}

const ReadingBookStats* ReadingStatsStore::findBook(const std::string& key) const {
  const size_t byPath = findBookIndexByPath(key);
  if (byPath < books.size()) return &books[byPath];
  const size_t byId = findBookIndexById(key);
  return byId < books.size() ? &books[byId] : nullptr;
}

const ReadingBookStats* ReadingStatsStore::findMatchingBookForPath(const std::string& path, const std::string& title,
                                                                   const std::string& author) const {
  if (const auto* exact = findBook(path)) return exact;
  if (const auto* same = findBook(BookIdentity::calculateContentBookId(path))) return same;
  for (const auto& book : books)
    if (!title.empty() && book.title == title && (author.empty() || book.author.empty() || book.author == author)) return &book;
  return nullptr;
}

bool ReadingStatsStore::updateBookMetadata(const std::string& path, const std::string& title, const std::string& author,
                                           const std::string& coverBmpPath) {
  const size_t index = findBookIndexByPath(path);
  if (index >= books.size()) return false;
  auto& book = books[index];
  bool changed = false;
  if (!title.empty() && book.title != title) { book.title = title; changed = true; }
  if (!author.empty() && book.author != author) { book.author = author; changed = true; }
  if (!coverBmpPath.empty() && book.coverBmpPath != coverBmpPath) { book.coverBmpPath = coverBmpPath; changed = true; }
  if (changed) { markDirty(); saveToFile(); }
  return changed;
}

bool ReadingStatsStore::updateBookPath(const std::string& oldKey, const std::string& newPath, const std::string& title,
                                       const std::string& author, const std::string& coverBmpPath,
                                       const std::string& bookId) {
  const std::string normalized = BookIdentity::normalizePath(newPath);
  size_t index = findBookIndexByPath(oldKey);
  if (index >= books.size() && !bookId.empty()) index = findBookIndexById(bookId);
  if (index >= books.size() || normalized.empty() || shouldIgnorePath(normalized)) return false;
  auto& book = books[index];
  if (!book.path.empty() && std::find(book.knownPaths.begin(), book.knownPaths.end(), book.path) == book.knownPaths.end()) book.knownPaths.push_back(book.path);
  if (std::find(book.knownPaths.begin(), book.knownPaths.end(), normalized) == book.knownPaths.end()) book.knownPaths.push_back(normalized);
  book.path = normalized;
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  if (!coverBmpPath.empty()) book.coverBmpPath = coverBmpPath;
  if (!bookId.empty() && (book.bookId.empty() || BookIdentity::isLegacyBookId(book.bookId))) book.bookId = bookId;
  if (!lastSessionSnapshot.path.empty() && findBookIndexByPath(lastSessionSnapshot.path) == index) {
    lastSessionSnapshot.path = normalized;
  }
  markDirty();
  return saveToFile();
}

bool ReadingStatsStore::updateBookPathPrefix(const std::string& oldPrefix, const std::string& newPrefix) {
  const std::string oldBase = BookIdentity::normalizePath(oldPrefix);
  const std::string newBase = BookIdentity::normalizePath(newPrefix);
  if (oldBase.empty() || newBase.empty()) return false;
  bool changed = false;
  for (auto& book : books) {
    const std::string current = BookIdentity::normalizePath(book.path);
    if (current != oldBase && current.rfind(oldBase + "/", 0) != 0) continue;
    const std::string rebased = newBase + current.substr(oldBase.size());
    book.path = rebased;
    for (auto& knownPath : book.knownPaths) {
      const std::string normalizedKnown = BookIdentity::normalizePath(knownPath);
      if (normalizedKnown == oldBase || normalizedKnown.rfind(oldBase + "/", 0) == 0)
        knownPath = newBase + normalizedKnown.substr(oldBase.size());
    }
    std::sort(book.knownPaths.begin(), book.knownPaths.end());
    book.knownPaths.erase(std::unique(book.knownPaths.begin(), book.knownPaths.end()), book.knownPaths.end());
    changed = true;
  }
  if (!lastSessionSnapshot.path.empty()) {
    const std::string current = BookIdentity::normalizePath(lastSessionSnapshot.path);
    if (current == oldBase || current.rfind(oldBase + "/", 0) == 0) {
      lastSessionSnapshot.path = newBase + current.substr(oldBase.size());
      changed = true;
    }
  }
  if (changed) { markDirty(); return saveToFile(); }
  return true;
}

bool ReadingStatsStore::removeBook(const std::string& path) {
  const size_t index = findBookIndexByPath(path);
  if (index >= books.size()) return false;
  books.erase(books.begin() + static_cast<std::ptrdiff_t>(index));
  if (activeSession.active) {
    if (activeSession.bookIndex == index) activeSession = {};
    else if (activeSession.bookIndex > index) --activeSession.bookIndex;
  }
  rebuildAggregatedDays();
  markDirty();
  saveToFile();
  return true;
}

bool ReadingStatsStore::adjustBookReadingTime(const std::string& path, const uint32_t ordinal, const int32_t deltaMs) {
  const size_t index = findBookIndexByPath(path);
  if (index >= books.size() || !ordinal || !deltaMs) return false;
  auto& book = books[index];
  if (deltaMs > 0) {
    addDay(book.readingDays, ordinal, static_cast<uint64_t>(deltaMs));
    book.totalReadingMs += static_cast<uint64_t>(deltaMs);
  } else {
    const uint64_t amount = static_cast<uint64_t>(-static_cast<int64_t>(deltaMs));
    auto it = std::find_if(book.readingDays.begin(), book.readingDays.end(), [ordinal](const auto& day) { return day.dayOrdinal == ordinal; });
    if (it == book.readingDays.end() || it->readingMs < amount || book.totalReadingMs < amount) return false;
    it->readingMs -= amount;
    if (!it->readingMs) book.readingDays.erase(it);
    book.totalReadingMs -= amount;
  }
  rebuildAggregatedDays();
  markDirty();
  return saveToFile();
}

uint32_t ReadingStatsStore::getBooksFinishedCount() const {
  return static_cast<uint32_t>(std::count_if(books.begin(), books.end(), [](const auto& book) { return book.completed; }));
}

uint32_t ReadingStatsStore::getReferenceDayOrdinal() const {
  const uint32_t timestamp = referenceTimestamp(TimeUtils::getAuthoritativeTimestamp());
  return isClockValid(timestamp) ? TimeUtils::getLocalDayOrdinal(timestamp) : (readingDays.empty() ? 0 : readingDays.back().dayOrdinal);
}

uint64_t ReadingStatsStore::getTotalReadingMs() const {
  uint64_t total = 0;
  for (const auto& book : books) total += book.totalReadingMs;
  return total;
}

uint64_t ReadingStatsStore::getTodayReadingMs() const {
  const uint32_t ordinal = getReferenceDayOrdinal();
  for (const auto& day : readingDays) if (day.dayOrdinal == ordinal) return day.readingMs;
  return 0;
}

uint64_t ReadingStatsStore::getRecentReadingMs(const uint32_t days) const {
  const uint32_t ordinal = getReferenceDayOrdinal();
  if (!ordinal || !days) return 0;
  const uint32_t first = ordinal >= days - 1 ? ordinal - days + 1 : 0;
  uint64_t total = 0;
  for (const auto& day : readingDays) if (day.dayOrdinal >= first && day.dayOrdinal <= ordinal) total += day.readingMs;
  return total;
}

uint32_t ReadingStatsStore::getCurrentStreakDays() const {
  const uint32_t ordinal = getReferenceDayOrdinal();
  if (!ordinal) return 0;
  uint32_t latestGoalDay = 0;
  for (const auto& day : readingDays)
    if (day.dayOrdinal <= ordinal && day.readingMs >= getDailyReadingGoalMs()) latestGoalDay = day.dayOrdinal;
  if (!latestGoalDay || (latestGoalDay != ordinal && latestGoalDay + 1 != ordinal)) return 0;

  uint32_t count = 0;
  for (uint32_t day = latestGoalDay;; --day) {
    const auto it = std::find_if(readingDays.begin(), readingDays.end(), [day](const auto& value) { return value.dayOrdinal == day; });
    if (it == readingDays.end() || it->readingMs < getDailyReadingGoalMs()) break;
    ++count;
    if (!day) break;
  }
  return count;
}

uint32_t ReadingStatsStore::getMaxStreakDays() const {
  uint32_t best = 0, run = 0, previous = std::numeric_limits<uint32_t>::max();
  for (const auto& day : readingDays) {
    if (day.readingMs >= getDailyReadingGoalMs() && previous != std::numeric_limits<uint32_t>::max() && day.dayOrdinal == previous + 1) ++run;
    else run = day.readingMs >= getDailyReadingGoalMs() ? 1 : 0;
    best = std::max(best, run);
    previous = day.dayOrdinal;
  }
  return best;
}

uint32_t ReadingStatsStore::getDisplayTimestamp(bool* usedFallback) const {
  const uint32_t current = TimeUtils::getAuthoritativeTimestamp();
  if (isClockValid(current)) { if (usedFallback) *usedFallback = false; return current; }
  const uint32_t latest = latestKnownTimestamp();
  if (usedFallback) *usedFallback = isClockValid(latest);
  return latest;
}

bool ReadingStatsStore::shouldSaveCheckpoint() const {
  if (!dirty || !activeSession.active) return false;
  const unsigned long now = millis();
  return now - dirtySinceMs >= CHECKPOINT_MS && (lastSaveAttemptMs == 0 || now - lastSaveAttemptMs >= RETRY_MS);
}

bool ReadingStatsStore::saveToFile() const {
  if (!dirty && Storage.exists(STATS_FILE)) return true;
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["formatVersion"] = 6;
  JsonArray booksArray = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject obj = booksArray.add<JsonObject>();
    obj["bookId"] = book.bookId; obj["path"] = book.path; obj["title"] = book.title; obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath; obj["chapterTitle"] = book.chapterTitle; obj["totalReadingMs"] = book.totalReadingMs;
    obj["sessions"] = book.sessions; obj["lastSessionMs"] = book.lastSessionMs; obj["firstReadAt"] = book.firstReadAt;
    obj["lastReadAt"] = book.lastReadAt; obj["completedAt"] = book.completedAt; obj["lastProgressPercent"] = book.lastProgressPercent;
    obj["chapterProgressPercent"] = book.chapterProgressPercent; obj["completed"] = book.completed;
    JsonArray paths = obj["knownPaths"].to<JsonArray>(); for (const auto& path : book.knownPaths) paths.add(path);
    appendDays(obj["readingDays"].to<JsonArray>(), book.readingDays);
  }
  appendDays(doc["readingDays"].to<JsonArray>(), readingDays);
  appendDays(doc["legacyReadingDays"].to<JsonArray>(), legacyReadingDays);
  JsonArray sessions = doc["sessionLog"].to<JsonArray>();
  for (const auto& session : sessionLog) { JsonObject obj = sessions.add<JsonObject>(); obj["dayOrdinal"] = session.dayOrdinal; obj["sessionMs"] = session.sessionMs; }
  lastSaveAttemptMs = millis();
  if (!writeDocument(STATS_TMP_FILE, doc)) return false;
  if (Storage.exists(STATS_FILE)) Storage.remove(STATS_FILE);
  const bool saved = Storage.rename(STATS_TMP_FILE, STATS_FILE);
  if (saved) { dirty = false; dirtySinceMs = 0; }
  return saved;
}

bool ReadingStatsStore::loadFromFile() {
  if (loaded) return true;
  loaded = true; books.clear(); readingDays.clear(); legacyReadingDays.clear(); sessionLog.clear(); dirty = false;
  if (!Storage.exists(STATS_FILE) && Storage.exists(STATS_TMP_FILE)) Storage.rename(STATS_TMP_FILE, STATS_FILE);
  if (!Storage.exists(STATS_FILE)) return false;
  const String json = Storage.readFile(STATS_FILE);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) { LOG_ERR("RST", "Failed to parse reading stats"); return false; }
  const uint32_t version = doc["formatVersion"] | static_cast<uint32_t>(1);
  if (version >= 2) {
    parseDays(doc["readingDays"].as<JsonArray>(), readingDays);
    parseDays(doc["legacyReadingDays"].as<JsonArray>(), legacyReadingDays);
  } else {
    // Version 1 stored aggregate days without book ownership. Preserve them
    // as unassigned days instead of dropping them during the v6 migration.
    parseDays(doc["readingDays"].as<JsonArray>(), legacyReadingDays);
  }
  if (version >= 4) {
    for (JsonObject obj : doc["sessionLog"].as<JsonArray>()) {
      sessionLog.push_back({obj["dayOrdinal"] | static_cast<uint32_t>(0), obj["sessionMs"] | static_cast<uint32_t>(0)});
    }
  }
  for (JsonObject obj : doc["books"].as<JsonArray>()) {
    ReadingBookStats book;
    book.bookId = obj["bookId"] | std::string(""); book.path = obj["path"] | std::string("");
    if (book.path.empty()) continue;
    book.title = obj["title"] | std::string(""); book.author = obj["author"] | std::string(""); book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    book.chapterTitle = obj["chapterTitle"] | std::string(""); book.totalReadingMs = obj["totalReadingMs"] | static_cast<uint64_t>(0);
    if (!book.totalReadingMs) book.totalReadingMs = (obj["totalSeconds"] | static_cast<uint64_t>(0)) * 1000ULL;
    book.sessions = obj["sessions"] | 0; book.lastSessionMs = obj["lastSessionMs"] | 0; book.firstReadAt = obj["firstReadAt"] | 0;
    book.lastReadAt = obj["lastReadAt"] | 0; book.completedAt = obj["completedAt"] | 0; book.lastProgressPercent = obj["lastProgressPercent"] | 0;
    book.chapterProgressPercent = obj["chapterProgressPercent"] | 0; book.completed = obj["completed"] | false;
    for (JsonVariant value : obj["knownPaths"].as<JsonArray>()) book.knownPaths.push_back(value | std::string(""));
    if (version >= 2) parseDays(obj["readingDays"].as<JsonArray>(), book.readingDays);
    books.push_back(std::move(book));
  }
  normalize();
  rebuildAggregatedDays();
  dirty = version < 6;
  if (dirty) saveToFile();
  LOG_DBG("RST", "Loaded reading stats (%d books)", static_cast<int>(books.size()));
  return true;
}

void ReadingStatsStore::reset() {
  books.clear(); legacyReadingDays.clear(); readingDays.clear(); sessionLog.clear(); activeSession = {}; lastSessionSnapshot = {};
  loaded = true; markDirty(); saveToFile();
}

void ReadingStatsStore::clear() {
  books.clear();
  legacyReadingDays.clear();
  readingDays.clear();
  sessionLog.clear();
  activeSession = {};
  lastSessionSnapshot = {};
  loaded = true;
  dirty = false;
  if (Storage.exists(STATS_FILE) && !Storage.remove(STATS_FILE)) LOG_ERR("RST", "Failed to remove reading stats");
  if (Storage.exists(STATS_TMP_FILE) && !Storage.remove(STATS_TMP_FILE)) LOG_ERR("RST", "Failed to remove stats temp file");
}

bool ReadingStatsStore::exportToFile(const std::string& path) const {
  if (path.empty()) return false;
  JsonDocument doc;
  doc["formatVersion"] = 6;
  JsonArray array = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject obj = array.add<JsonObject>(); obj["bookId"] = book.bookId; obj["path"] = book.path; obj["title"] = book.title;
    obj["totalReadingMs"] = book.totalReadingMs; obj["sessions"] = book.sessions; obj["completed"] = book.completed;
    appendDays(obj["readingDays"].to<JsonArray>(), book.readingDays);
  }
  appendDays(doc["readingDays"].to<JsonArray>(), readingDays);
  return writeDocument(path.c_str(), doc);
}

bool ReadingStatsStore::importFromFile(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) return false;
  if (Storage.exists(STATS_FILE)) Storage.remove(STATS_FILE);
  if (!Storage.writeFile(STATS_FILE, json)) return false;
  loaded = false;
  return loadFromFile();
}

const std::vector<ReadingStatsStore::LegacyEntry>& ReadingStatsStore::getEntries() const {
  legacyEntries.clear();
  for (const auto& book : books)
    legacyEntries.push_back({book.path, book.title, static_cast<uint32_t>(book.totalReadingMs / 1000ULL)});
  return legacyEntries;
}
