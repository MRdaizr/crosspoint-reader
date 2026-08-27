#include "ReadingStatsAnalytics.h"

#include <algorithm>

#include "TimeUtils.h"

namespace ReadingStatsAnalytics {

std::string formatDurationHm(const uint64_t milliseconds) {
  const uint64_t minutes = milliseconds / 60000ULL;
  if (minutes < 60) return std::to_string(minutes) + "m";
  return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
}

std::string formatDayOrdinalLabel(const uint32_t ordinal) {
  int year = 0;
  unsigned month = 0, day = 0;
  return TimeUtils::getDateFromDayOrdinal(ordinal, year, month, day) ? TimeUtils::formatDateParts(year, month, day) : "";
}

std::string formatMonthLabel(const int year, const unsigned month) { return TimeUtils::formatMonthYear(year, month); }

int getReferenceYear() {
  const uint32_t timestamp = READING_STATS.getDisplayTimestamp();
  std::tm local{};
  if (TimeUtils::isClockValid(timestamp) && TimeUtils::getLocalDateTime(timestamp, local)) return local.tm_year + 1900;
  if (!READING_STATS.getReadingDays().empty()) {
    int year = 0;
    unsigned month = 0, day = 0;
    if (TimeUtils::getDateFromDayOrdinal(READING_STATS.getReadingDays().back().dayOrdinal, year, month, day)) return year;
  }
  return 2026;
}

std::vector<DayBookEntry> getBooksReadOnDay(const uint32_t ordinal) {
  std::vector<DayBookEntry> result;
  for (const auto& book : READING_STATS.getBooks()) {
    for (const auto& day : book.readingDays) {
      if (day.dayOrdinal == ordinal && day.readingMs >= 3ULL * 60ULL * 1000ULL) {
        result.push_back({&book, day.readingMs});
        break;
      }
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left.readingMs != right.readingMs) return left.readingMs > right.readingMs;
    return left.book && right.book ? left.book->title < right.book->title : left.book != nullptr;
  });
  return result;
}

TimelineDayEntry buildTimelineDayEntry(const uint32_t ordinal) {
  TimelineDayEntry result;
  result.dayOrdinal = ordinal;
  for (const auto& day : READING_STATS.getReadingDays()) {
    if (day.dayOrdinal == ordinal) {
      result.totalReadingMs = day.readingMs;
      break;
    }
  }
  const auto books = getBooksReadOnDay(ordinal);
  result.booksReadCount = static_cast<uint32_t>(books.size());
  if (!books.empty()) {
    result.topBook = books.front().book;
    result.topBookReadingMs = books.front().readingMs;
  }
  return result;
}

std::vector<TimelineDayEntry> buildTimelineEntries(const size_t maxEntries) {
  std::vector<TimelineDayEntry> result;
  const auto& days = READING_STATS.getReadingDays();
  result.reserve(days.size());
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (!it->readingMs) continue;
    result.push_back(buildTimelineDayEntry(it->dayOrdinal));
    if (maxEntries && result.size() >= maxEntries) break;
  }
  return result;
}

}  // namespace ReadingStatsAnalytics
