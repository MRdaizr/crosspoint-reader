#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

std::string getBookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return {};
}

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
    Epub unified(path, "/.crosspoint");
    unified.clearCache();
    // Synthetic clearCache intentionally retains positions for re-conversion;
    // an explicit deletion must remove both namespaces, including those records.
    if (Storage.exists(unified.getCachePath().c_str())) Storage.removeDir(unified.getCachePath().c_str());
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

namespace {
bool invalidateDirectory(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return true;
  auto directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) return false;
  auto name = makeUniqueNoThrow<char[]>(256);
  if (!name) return false;
  bool ok = true;
  for (auto file = directory.openNextFile(); file; file = directory.openNextFile()) {
    file.getName(name.get(), 256);
    const bool folder = file.isDirectory();
    file.close();
    const std::string_view entry(name.get());
    if (entry.starts_with("progress") || entry.starts_with("legacy_progress") || entry.starts_with("txt_progress") ||
        entry == "bookmarks.bin")
      continue;
    const std::string target = path + "/" + name.get();
    if (!(folder ? Storage.removeDir(target.c_str()) : Storage.remove(target.c_str()))) ok = false;
  }
  return ok;
}
}  // namespace

bool invalidateBookCacheDirectory(const std::string& path) {
  constexpr std::string_view root = "/.crosspoint/";
  const std::string_view view(path);
  if (!view.starts_with(root)) return false;
  const auto name = view.substr(root.size());
  if (name.empty() || name.find_first_of("/\\.") != std::string_view::npos ||
      !isBookCacheDirectoryName(path.c_str() + root.size()))
    return false;
  return invalidateDirectory(path);
}

bool invalidateBookCache(const std::string& path) {
  bool ok = invalidateDirectory(getBookCachePath(path));
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    ok = invalidateDirectory(Epub(path, "/.crosspoint").getCachePath()) && ok;
  }
  return ok;
}

void relocateAdditionalTextCache(const std::string& oldPath, const std::string& newPath) {
  if (!(FsHelpers::hasTxtExtension(oldPath) || FsHelpers::hasMarkdownExtension(oldPath)) ||
      !(FsHelpers::hasTxtExtension(newPath) || FsHelpers::hasMarkdownExtension(newPath)))
    return;
  const std::string oldCache = Epub(oldPath, "/.crosspoint").getCachePath();
  const std::string newCache = Epub(newPath, "/.crosspoint").getCachePath();
  if (oldCache == newCache || !Storage.exists(oldCache.c_str())) return;
  if (Storage.exists(newCache.c_str()) || !Storage.rename(oldCache.c_str(), newCache.c_str())) {
    LOG_ERR("BookCache", "Could not relocate unified TXT cache");
  }
}
