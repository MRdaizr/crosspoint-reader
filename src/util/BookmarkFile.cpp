#include "BookmarkFile.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

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
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    LOG_ERR("BKM", "Empty bookmark file: %s", path.c_str());
    return false;
  }
  if (!JsonSettingsIO::loadBookmarks(bookmarks, json.c_str())) return false;

  // Bookmark files are user-editable and can be copied from another device.
  // Keep malformed/oversized records from turning a later render into a large
  // heap allocation or an invalid page lookup.
  constexpr size_t MAX_BOOKMARKS = 256;
  constexpr size_t MAX_XPATH_LENGTH = 512;
  constexpr size_t MAX_SUMMARY_LENGTH = 128;
  if (bookmarks.size() > MAX_BOOKMARKS) {
    LOG_ERR("BKM", "Too many bookmarks (%zu), truncating to %zu", bookmarks.size(), MAX_BOOKMARKS);
    bookmarks.resize(MAX_BOOKMARKS);
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
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return JsonSettingsIO::saveBookmarks(bookmarks, path.c_str());
}
