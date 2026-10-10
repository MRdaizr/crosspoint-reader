#pragma once
#include "CacheModel.h"
#include "FsHelpers.h"
class Epub {
  std::string path;
  bool synthetic;

 public:
  Epub(const std::string& book, const std::string& base)
      : path(cachetest::cachePath("epub", book, base)),
        synthetic(FsHelpers::hasTxtExtension(book) || FsHelpers::hasMarkdownExtension(book)) {}
  const std::string& getCachePath() const { return path; }
  bool clearCache() const {
    if (!synthetic) return cachetest::clearAll("epub", path);
    // Match the production engine boundary for synthetic TXT: clear layouts
    // and manifest, retaining resume records. Dispatch is the unit under test.
    cachetest::clearCalls.push_back({"epub", path});
    const std::string sections = path + "/sections", manifest = path + "/text.manifest";
    if (Storage.exists(sections.c_str()) && !Storage.removeDir(sections.c_str())) return false;
    return !Storage.exists(manifest.c_str()) || Storage.remove(manifest.c_str());
  }
};
