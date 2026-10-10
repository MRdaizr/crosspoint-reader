#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

// Never hand an EPUB URI to an external protocol handler. Also reject controls,
// leading whitespace, network paths and all schemes (including mixed case).
inline bool isInternalPageLink(const char* href) {
  if (!href || !*href || static_cast<unsigned char>(*href) <= ' ' || href[0] == '/' || href[0] == '\\') return false;
  const size_t length = strnlen(href, 513);
  if (length == 0 || length > 512) return false;
  bool fragment = false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(href[i]);
    if (c < ' ' || c == 127 || c == '\\') return false;
    if (c == '#') fragment = true;
    if (c == ':' && !fragment) return false;
  }
  return true;
}

struct PageLink {
  static constexpr uint16_t MAX_TARGET_BYTES = 512;
  static constexpr uint16_t MAX_PER_PAGE = 32;
  static constexpr int MAX_COORDINATE = 8192;
  std::unique_ptr<char[]> href;
  uint32_t identity = 0;  // Source anchor identity, not a hash of its target.
  int16_t x = 0, y = 0, width = 0, height = 0;

  bool setTarget(const char* target) {
    if (!isInternalPageLink(target)) return false;
    const size_t length = strlen(target);
    auto copy = makeUniqueNoThrow<char[]>(length + 1);
    if (!copy) {
      LOG_ERR("LNK", "OOM: link target");
      return false;
    }
    memcpy(copy.get(), target, length + 1);
    href = std::move(copy);
    return true;
  }
  bool validGeometry() const {
    return identity != 0 && href && isInternalPageLink(href.get()) && width > 0 && height > 0 &&
           width <= MAX_COORDINATE && height <= MAX_COORDINATE && x >= -MAX_COORDINATE && y >= -MAX_COORDINATE &&
           static_cast<int>(x) + width <= MAX_COORDINATE && static_cast<int>(y) + height <= MAX_COORDINATE;
  }
  bool contains(int px, int py, int slop = 0, int minWidth = 0) const {
    const int sx = std::max(slop, (minWidth - width) / 2);
    return px >= x - sx && px < static_cast<int>(x) + width + sx && py >= y - slop &&
           py < static_cast<int>(y) + height + slop;
  }
  bool serialize(HalFile& file) const;
  bool deserialize(HalFile& file);
};

// Checked, lazily grown storage: no std::vector allocation can abort on OOM.
// Pages cap geometry at 32; a streaming paragraph may retain 255 target IDs.
// Targets are variable length rather than a 513-byte array in every entry.
class PageLinks {
  std::unique_ptr<PageLink[]> entries;
  uint16_t count = 0, capacity = 0;
  uint16_t limit = 32;

 public:
  explicit PageLinks(uint16_t limit = 32) : limit(limit) {}
  PageLinks(PageLinks&& other) noexcept { *this = std::move(other); }
  PageLinks& operator=(PageLinks&& other) noexcept {
    if (this != &other) {
      entries = std::move(other.entries);
      count = other.count;
      capacity = other.capacity;
      limit = other.limit;
      other.count = other.capacity = 0;
    }
    return *this;
  }
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  PageLink* begin() { return entries.get(); }
  const PageLink* begin() const { return entries.get(); }
  PageLink* end() { return count ? entries.get() + count : entries.get(); }
  const PageLink* end() const { return count ? entries.get() + count : entries.get(); }
  PageLink& operator[](size_t i) { return entries[i]; }
  const PageLink& operator[](size_t i) const { return entries[i]; }
  bool firstOccurrence(size_t i) const {
    for (size_t j = 0; j < i; ++j)
      if (entries[j].identity == entries[i].identity) return false;
    return true;
  }
  size_t uniqueCount() const {
    size_t result = 0;
    for (size_t i = 0; i < count; ++i)
      if (firstOccurrence(i)) ++result;
    return result;
  }
  int hitTest(int x, int y, int slop = 3, int minWidth = 12) const {
    for (int pass = 0; pass < 2; ++pass) {
      int best = -1;
      int bestArea = INT32_MAX;
      for (size_t i = 0; i < count; ++i) {
        const auto& link = entries[i];
        const int area = link.width * link.height;
        if (link.contains(x, y, pass ? slop : 0, pass ? minWidth : 0) && area < bestArea) {
          best = static_cast<int>(i);
          bestArea = area;
        }
      }
      if (best >= 0) return best;
    }
    return -1;
  }
  bool reserve(size_t requested) {
    if (requested > limit) return false;
    if (requested <= capacity) return true;
    auto grown = makeUniqueNoThrow<PageLink[]>(requested);
    if (!grown) {
      LOG_ERR("LNK", "OOM: link geometry");
      return false;
    }
    for (size_t i = 0; i < count; ++i) grown[i] = std::move(entries[i]);
    entries = std::move(grown);
    capacity = static_cast<uint16_t>(requested);
    return true;
  }
  bool append(PageLink&& link) {
    if (count >= limit) return false;
    if (count == capacity && !reserve(std::min<size_t>(limit, capacity ? capacity * 2 : 4))) return false;
    entries[count++] = std::move(link);
    return true;
  }
};
