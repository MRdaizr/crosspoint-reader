#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace wallpaperfake {
inline std::map<std::string, std::string> files;
inline int failRenameAfter = -1;
inline bool failWrite = false, failClose = false, failRead = false;
inline void reset() {
  files.clear();
  failRenameAfter = -1;
  failWrite = failClose = failRead = false;
}
}  // namespace wallpaperfake

class HalFile {
 public:
  std::string path;
  size_t offset = 0;
  bool opened = false;
  int available() const { return opened ? static_cast<int>(wallpaperfake::files[path].size() - offset) : 0; }
  uint64_t fileSize64() const { return wallpaperfake::files[path].size(); }
  int read(void* data, size_t count) {
    if (wallpaperfake::failRead) {
      wallpaperfake::failRead = false;
      return -1;
    }
    if (!opened) return -1;
    const auto& bytes = wallpaperfake::files[path];
    count = std::min(count, bytes.size() - offset);
    memcpy(data, bytes.data() + offset, count);
    offset += count;
    return static_cast<int>(count);
  }
  size_t write(const void* data, size_t count) {
    if (wallpaperfake::failWrite) {
      wallpaperfake::failWrite = false;
      return 0;
    }
    wallpaperfake::files[path].append(static_cast<const char*>(data), count);
    return count;
  }
  void flush() {}
  bool close() {
    opened = false;
    if (wallpaperfake::failClose) {
      wallpaperfake::failClose = false;
      return false;
    }
    return true;
  }
};
struct FakeStorage {
  bool exists(const char* path) const { return wallpaperfake::files.count(path) != 0; }
  bool remove(const char* path) { return wallpaperfake::files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (wallpaperfake::failRenameAfter == 0) {
      wallpaperfake::failRenameAfter = -1;
      return false;
    }
    if (wallpaperfake::failRenameAfter > 0) --wallpaperfake::failRenameAfter;
    if (!exists(from) || exists(to)) return false;
    wallpaperfake::files[to] = wallpaperfake::files[from];
    wallpaperfake::files.erase(from);
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (!exists(path)) return false;
    file.path = path;
    file.offset = 0;
    file.opened = true;
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    wallpaperfake::files[path].clear();
    file.path = path;
    file.offset = 0;
    file.opened = true;
    return true;
  }
};
inline FakeStorage Storage;
