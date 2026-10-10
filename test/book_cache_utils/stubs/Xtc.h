#pragma once
#include "CacheModel.h"
class Xtc {
  std::string path;

 public:
  Xtc(const std::string& book, const std::string& base) : path(cachetest::cachePath("xtc", book, base)) {}
  const std::string& getCachePath() const { return path; }
  bool clearCache() const { return cachetest::clearAll("xtc", path); }
};
