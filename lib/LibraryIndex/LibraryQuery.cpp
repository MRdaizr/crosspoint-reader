#include "LibraryQuery.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstring>

#include "LibraryText.h"
#ifdef ARDUINO
#include "../../src/util/TaskWatchdog.h"
#endif

namespace library {
bool LibraryQuery::readDisplay(LibraryIndexFile& index, const ClixRecord& record, char* shownTitle,
                               const size_t titleCapacity, char* shownAuthor, const size_t authorCapacity) {
  if (!index.readTitle(record, shownTitle, titleCapacity) || !index.readAuthor(record, shownAuthor, authorCapacity))
    return false;
  if (!shownTitle[0]) {
    if (!index.readName(record, shownTitle, titleCapacity)) return false;
    char* dot = strrchr(shownTitle, '.');
    if (dot && dot != shownTitle) *dot = '\0';
  }
  return true;
}

bool LibraryQuery::matches(LibraryIndexFile& index, const ClixRecord& record, const std::string_view foldedQuery) {
  if (foldedQuery.empty()) return true;
  if (!readDisplay(index, record, title, sizeof(title), author, sizeof(author))) return false;
  size_t used = foldInto(title, folded, sizeof(folded));
  if (used + 1 < sizeof(folded)) folded[used++] = ' ';
  used += foldInto(author, folded + used, sizeof(folded) - used);
  return matchesQuery(std::string_view(folded, used), foldedQuery);
}

bool LibraryQuery::run(LibraryIndexFile& index, const std::string_view query, const SortOrder order,
                       const uint16_t offset, const uint16_t limit, QueryPage& page) {
  page = QueryPage{};
  if (!index.isOpen() || query.size() > LIBRARY_QUERY_BYTES || !limit || limit > LIBRARY_PAGE_LIMIT) {
    LOG_ERR("LIBQ", "invalid query, page limit, or closed index");
    return false;
  }
  const size_t needleLen = foldInto(query, needle, sizeof(needle));
  page.degraded = index.ranksDegraded();
  const std::string_view foldedQuery(needle, needleLen);
  if (foldedQuery.empty()) {
    page.total = index.bookCount();
    for (uint16_t row = offset; row < index.bookCount() && page.count < limit; ++row) {
      const uint16_t ordinal = index.ordinalForRow(order, row);
      if (ordinal == 0xFFFF) return false;
      page.ordinals[page.count++] = ordinal;
    }
    return true;  // ordinary shelves touch only the requested page
  }
  for (uint16_t row = 0; row < index.bookCount(); ++row) {
    if ((row & 31u) == 0) {
#ifdef ARDUINO
      resetTaskWatchdogIfSubscribed();
#endif
      delay(1);
    }
    const uint16_t ordinal = index.ordinalForRow(order, row);
    ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return false;
    const bool hit = matches(index, record, foldedQuery);
    if (index.ioFailed()) return false;
    if (!hit) continue;
    if (page.total >= offset && page.count < limit) page.ordinals[page.count++] = ordinal;
    ++page.total;
  }
  return true;
}
}  // namespace library
