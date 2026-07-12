#include "FlashcardScheduler.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
constexpr uint32_t MAGIC = 0x31525346;  // FSR1
constexpr uint16_t VERSION = 4;
constexpr uint16_t DEFAULT_EASE = 2500;
constexpr uint16_t MIN_EASE = 1300;
constexpr uint32_t MINUTE = 60;
constexpr uint32_t DAY = 24 * 60 * 60;

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  char dailyDate[11];
  uint16_t newCardsToday;
  uint16_t newCardLimitToday;
  uint16_t reviewsToday;
  uint16_t completedToday;
  char lastStudyDate[11];
  uint16_t currentStreak;
  uint16_t maxStreak;
  uint32_t totalReviews;
};
#pragma pack(pop)

std::string dateFor(const time_t now) {
  if (now < FlashcardScheduler::VALID_EPOCH) return {};
  struct tm local {};
  localtime_r(&now, &local);
  char value[11];
  snprintf(value, sizeof(value), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  return value;
}
std::string today() { return dateFor(time(nullptr)); }
}  // namespace

uint64_t FlashcardScheduler::cardId(const std::string& word, const std::string& phonetic, const std::string& definition) {
  uint64_t hash = 1469598103934665603ULL;
  const auto add = [&hash](const std::string& text) {
    for (const unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ULL; }
    hash ^= 0x1F; hash *= 1099511628211ULL;
  };
  add(word); add(phonetic); add(definition);
  return hash;
}

bool FlashcardScheduler::hasValidTime() const { return time(nullptr) >= VALID_EPOCH; }

FlashcardSrsRecord* FlashcardScheduler::find(uint64_t cardId) {
  const auto it = std::lower_bound(records.begin(), records.end(), cardId,
                                   [](const FlashcardSrsRecord& record, uint64_t id) { return record.cardId < id; });
  return it != records.end() && it->cardId == cardId ? &*it : nullptr;
}
const FlashcardSrsRecord* FlashcardScheduler::find(uint64_t cardId) const {
  return const_cast<FlashcardScheduler*>(this)->find(cardId);
}

bool FlashcardScheduler::load(const std::string& deckPath) {
  const uint64_t deckHash = cardId(deckPath, "", "");
  char filename[48];
  snprintf(filename, sizeof(filename), "/.crosspoint/flashcards/%08lx.srs", static_cast<unsigned long>(deckHash));
  statePath = filename;
  records.clear(); dailyDate = today(); newCardsToday = 0; newCardLimitToday = DAILY_NEW_LIMIT; reviewsToday = 0; completedToday = 0; loaded = true;
  Storage.mkdir("/.crosspoint/flashcards");
  HalFile file;
  if (!Storage.openFileForRead("SRS", statePath, file)) return true;
  Header header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || header.magic != MAGIC || header.version != VERSION) {
    file.close();
    LOG_ERR("SRS", "Invalid schedule file: %s", statePath.c_str());
    return true;
  }
  dailyDate = header.dailyDate;
  newCardsToday = header.newCardsToday;
  newCardLimitToday = header.newCardLimitToday;
  reviewsToday = header.reviewsToday;
  completedToday = header.completedToday;
  lastStudyDate = header.lastStudyDate;
  currentStreak = header.currentStreak;
  maxStreak = header.maxStreak;
  totalReviews = header.totalReviews;
  records.resize(header.count);
  if (!records.empty() && file.read(records.data(), records.size() * sizeof(FlashcardSrsRecord)) !=
                              static_cast<int>(records.size() * sizeof(FlashcardSrsRecord))) {
    records.clear();
    LOG_ERR("SRS", "Truncated schedule file: %s", statePath.c_str());
  }
  file.close();
  std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.cardId < b.cardId; });
  if (dailyDate != today()) { dailyDate = today(); newCardsToday = 0; newCardLimitToday = DAILY_NEW_LIMIT; reviewsToday = 0; completedToday = 0; save(); }
  return true;
}

bool FlashcardScheduler::save() const {
  if (!loaded) return false;
  const std::string temp = statePath + ".tmp";
  Storage.remove(temp.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("SRS", temp, file)) return false;
  Header header{MAGIC, VERSION, static_cast<uint16_t>(records.size()), {}, newCardsToday, newCardLimitToday, reviewsToday, completedToday,
                {}, currentStreak, maxStreak, totalReviews};
  snprintf(header.dailyDate, sizeof(header.dailyDate), "%s", dailyDate.c_str());
  snprintf(header.lastStudyDate, sizeof(header.lastStudyDate), "%s", lastStudyDate.c_str());
  const bool ok = file.write(&header, sizeof(header)) == sizeof(header) &&
                  (records.empty() || file.write(records.data(), records.size() * sizeof(FlashcardSrsRecord)) ==
                                      records.size() * sizeof(FlashcardSrsRecord));
  file.close();
  if (!ok) { Storage.remove(temp.c_str()); return false; }
  Storage.remove(statePath.c_str());
  return Storage.rename(temp.c_str(), statePath.c_str());
}

uint16_t FlashcardScheduler::newCardsRemaining() {
  if (dailyDate != today()) { dailyDate = today(); newCardsToday = 0; newCardLimitToday = DAILY_NEW_LIMIT; reviewsToday = 0; completedToday = 0; save(); }
  return newCardsToday >= newCardLimitToday ? 0 : newCardLimitToday - newCardsToday;
}
bool FlashcardScheduler::unlockNextNewBatch() {
  if (!hasValidTime()) return false;
  if (dailyDate != today()) { dailyDate = today(); newCardsToday = 0; newCardLimitToday = DAILY_NEW_LIMIT; reviewsToday = 0; completedToday = 0; }
  newCardLimitToday = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, newCardLimitToday + DAILY_NEW_LIMIT));
  return save();
}
uint16_t FlashcardScheduler::reviewCardsRemaining() {
  if (dailyDate != today()) { dailyDate = today(); newCardsToday = 0; newCardLimitToday = DAILY_NEW_LIMIT; reviewsToday = 0; completedToday = 0; save(); }
  return reviewsToday >= DAILY_REVIEW_LIMIT ? 0 : DAILY_REVIEW_LIMIT - reviewsToday;
}
const FlashcardSrsRecord* FlashcardScheduler::get(uint64_t cardId) const { return find(cardId); }
bool FlashcardScheduler::isDue(uint64_t cardId, time_t now) const { const auto* r = find(cardId); return r && r->dueAt <= now; }
bool FlashcardScheduler::isLearningDue(uint64_t cardId, time_t now) const {
  const auto* r = find(cardId); return r && r->dueAt <= now && r->state != static_cast<uint8_t>(FlashcardSrsState::REVIEW);
}
bool FlashcardScheduler::introduce(uint64_t id) {
  if (!hasValidTime() || find(id) || newCardsRemaining() == 0) return false;
  records.push_back({id, static_cast<uint32_t>(time(nullptr)), 0, DEFAULT_EASE, 0, 0,
                     static_cast<uint8_t>(FlashcardSrsState::LEARNING), 0});
  std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.cardId < b.cardId; });
  ++newCardsToday;
  return save();
}
bool FlashcardScheduler::grade(uint64_t id, FlashcardGrade grade) {
  auto* r = find(id);
  if (!r || !hasValidTime()) return false;
  const time_t now = time(nullptr);
  const auto state = static_cast<FlashcardSrsState>(r->state);
  if (grade == FlashcardGrade::AGAIN) {
    if (state == FlashcardSrsState::REVIEW) { ++r->lapses; r->easePermille = std::max<uint16_t>(MIN_EASE, r->easePermille - 200); }
    r->state = static_cast<uint8_t>(FlashcardSrsState::RELEARNING); r->step = 0; r->dueAt = now + MINUTE;
  } else if (state != FlashcardSrsState::REVIEW) {
    if (grade == FlashcardGrade::HARD) { r->dueAt = now + (r->step == 0 ? 6 * MINUTE : r->step == 1 ? 10 * MINUTE : DAY); }
    else if (++r->step >= 3) { r->state = static_cast<uint8_t>(FlashcardSrsState::REVIEW); r->intervalDays = 1; r->dueAt = now + DAY; ++r->repetitions; }
    else r->dueAt = now + (r->step == 1 ? 10 * MINUTE : DAY);
  } else if (grade == FlashcardGrade::HARD) {
    r->easePermille = std::max<uint16_t>(MIN_EASE, r->easePermille - 150);
    r->intervalDays = std::max<uint16_t>(1, static_cast<uint16_t>((r->intervalDays * 12 + 9) / 10));
    r->dueAt = now + static_cast<uint32_t>(r->intervalDays) * DAY; ++r->repetitions;
  } else {
    r->intervalDays = std::max<uint16_t>(1, static_cast<uint16_t>((r->intervalDays * r->easePermille + 999) / 1000));
    r->dueAt = now + static_cast<uint32_t>(r->intervalDays) * DAY; ++r->repetitions;
  }
  if (state == FlashcardSrsState::REVIEW) ++reviewsToday;
  ++completedToday;
  ++totalReviews;
  const std::string currentDate = today();
  if (lastStudyDate != currentDate) {
    currentStreak = !lastStudyDate.empty() && lastStudyDate == dateFor(time(nullptr) - DAY) ? currentStreak + 1 : 1;
    lastStudyDate = currentDate;
    maxStreak = std::max(maxStreak, currentStreak);
  }
  return save();
}
uint32_t FlashcardScheduler::learnedCount() const { return static_cast<uint32_t>(records.size()); }
uint32_t FlashcardScheduler::dueReviewCount() const { const time_t now=time(nullptr); uint32_t count=0; for (const auto& r:records) if(r.state==static_cast<uint8_t>(FlashcardSrsState::REVIEW)&&r.dueAt<=now) ++count; return count; }
uint32_t FlashcardScheduler::dueCountWithinDays(const uint8_t days) const {
  const time_t now = time(nullptr);
  const time_t end = now + static_cast<time_t>(days) * DAY;
  uint32_t count = 0;
  for (const auto& record : records) if (record.dueAt >= now && record.dueAt < end) ++count;
  return count;
}
