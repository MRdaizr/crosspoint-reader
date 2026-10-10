#pragma once
#include <functional>
#include <string>
#include <vector>

#include "HalStorage.h"

namespace cachetest {
struct ClearCall {
  std::string kind, path;
};
inline std::vector<ClearCall> clearCalls;
inline std::string cachePath(const std::string& kind, const std::string& book, const std::string& base) {
  // Same path/namespace contract as Epub/Txt/Xtc, not a platform-specific
  // fixture hash constant. Tests exercise BookCacheUtils, not engine parsing.
  return base + "/" + kind + "_" + std::to_string(std::hash<std::string>{}(book));
}
inline bool clearAll(const std::string& kind, const std::string& path) {
  clearCalls.push_back({kind, path});
  return !Storage.exists(path.c_str()) || Storage.removeDir(path.c_str());
}
}  // namespace cachetest
