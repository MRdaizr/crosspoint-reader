#pragma once
#include "CacheModel.h"
class Txt {
  std::string path;

 public:
  Txt(const std::string& book, const std::string& base) : path(cachetest::cachePath("txt", book, base)) {}
  const std::string& getCachePath() const { return path; }
  bool clearCache() const { return cachetest::clearAll("txt", path); }
};
