#pragma once
#include <cctype>
#include <cstring>
#include <string_view>
namespace FsHelpers {
inline bool checkFileExtension(std::string_view path, const char* extension) {
  const size_t length = std::strlen(extension);
  if (path.size() < length) return false;
  path.remove_prefix(path.size() - length);
  for (size_t i = 0; i < length; ++i)
    if (std::tolower(static_cast<unsigned char>(path[i])) != std::tolower(static_cast<unsigned char>(extension[i])))
      return false;
  return true;
}
inline bool hasEpubExtension(std::string_view path) { return checkFileExtension(path, ".epub"); }
}  // namespace FsHelpers
