#include "BookmarkFile.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  if (!Storage.exists(path.c_str())) return false;
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    LOG_ERR("BKM", "Empty bookmark file: %s", path.c_str());
    return false;
  }
  return JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  const std::string dir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(dir.c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return JsonSettingsIO::saveBookmarks(bookmarks, path.c_str());
}
