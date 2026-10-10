#pragma once

#include <cstdint>
#include <string_view>

#include "LibraryIndexFile.h"

namespace library {
inline constexpr uint16_t LIBRARY_PAGE_LIMIT = 32;
inline constexpr size_t LIBRARY_QUERY_BYTES = 128;
// Ordinals only: callers materialize their visible page, never the full shelf.
struct QueryPage {
  uint16_t ordinals[LIBRARY_PAGE_LIMIT] = {};
  uint16_t count = 0;
  uint16_t total = 0;
  bool degraded = false;
};

// Allocate one fallible instance on the caller's heap; its reusable scratch
// avoids large stack frames and all string allocations inside the scan loop.
class LibraryQuery {
 public:
  bool run(LibraryIndexFile& index, std::string_view query, SortOrder order, uint16_t offset, uint16_t limit,
           QueryPage& page);
  bool matches(LibraryIndexFile& index, const ClixRecord& record, std::string_view foldedQuery);
  bool readDisplay(LibraryIndexFile& index, const ClixRecord& record, char* title, size_t titleCapacity, char* author,
                   size_t authorCapacity);

 private:
  char needle[LIBRARY_QUERY_BYTES * 2] = {};
  char title[256] = {};
  char author[256] = {};
  char folded[768] = {};
};
}  // namespace library
