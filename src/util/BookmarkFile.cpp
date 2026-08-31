#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cmath>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  if (!Storage.exists(path.c_str())) return false;
  HalFile bookmarkFile;
  if (!Storage.openFileForRead("BKM", path, bookmarkFile)) return false;
  constexpr size_t MAX_BOOKMARK_FILE_BYTES = 64 * 1024;
  const size_t fileSize = bookmarkFile.fileSize();
  bookmarkFile.close();
  if (fileSize > MAX_BOOKMARK_FILE_BYTES) {
    LOG_ERR("BKM", "Bookmark file too large: %zu bytes", fileSize);
    return false;
  }
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;

  // Bookmark files are user-editable and can be copied from another device.
  // Keep malformed/oversized records from turning a later render into a large
  // heap allocation or an invalid page lookup.
  constexpr size_t MAX_BOOKMARKS = 256;
  constexpr size_t MAX_XPATH_LENGTH = 512;
  constexpr size_t MAX_SUMMARY_LENGTH = 128;
  const JsonArrayConst arr = doc["bookmarks"].as<JsonArrayConst>();
  bookmarks.reserve(std::min(arr.size(), MAX_BOOKMARKS));
  for (JsonObjectConst obj : arr) {
    if (bookmarks.size() >= MAX_BOOKMARKS) break;
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
      bookmark.hasVisibleTextOffset = true;
    }
  }
  if (arr.size() > MAX_BOOKMARKS) {
    LOG_ERR("BKM", "Too many bookmarks (%zu), truncating to %zu", arr.size(), MAX_BOOKMARKS);
  }
  for (auto& bookmark : bookmarks) {
    if (bookmark.xpath.size() > MAX_XPATH_LENGTH) bookmark.xpath.resize(MAX_XPATH_LENGTH);
    if (bookmark.summary.size() > MAX_SUMMARY_LENGTH) bookmark.summary.resize(MAX_SUMMARY_LENGTH);
    if (!std::isfinite(bookmark.percentage)) bookmark.percentage = 0.0f;
    bookmark.percentage = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  }
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  constexpr size_t MAX_BOOKMARKS = 256;
  if (bookmarks.size() > MAX_BOOKMARKS) {
    LOG_ERR("BKM", "Refusing to save %zu bookmarks (limit %zu)", bookmarks.size(), MAX_BOOKMARKS);
    return false;
  }
  const std::string dir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(dir.c_str());
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) obj["vo"] = bookmark.visibleTextOffset;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
