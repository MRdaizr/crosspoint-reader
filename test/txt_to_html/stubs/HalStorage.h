#pragma once
#include <Print.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>

struct TestRecord {
  std::string bytes;
  uint32_t time = 1;
};
class HalFile;
class HalStorage {
 public:
  std::map<std::string, TestRecord> files;
  std::set<std::string> directories;
  std::string failWrite, failRename, failClose;
  bool exactRename = false;
  int renameFailures = 0;
  size_t maxRead = SIZE_MAX, readBytes = 0, maxReadRequested = 0;
  static HalStorage& getInstance() {
    static HalStorage value;
    return value;
  }
  void reset() {
    files.clear();
    directories.clear();
    failWrite.clear();
    failRename.clear();
    failClose.clear();
    exactRename = false;
    renameFailures = 0;
    maxRead = SIZE_MAX;
    readBytes = maxReadRequested = 0;
  }
  bool exists(const char* path) const { return files.count(path) || directories.count(path); }
  bool mkdir(const char* path) {
    directories.insert(path);
    return true;
  }
  bool remove(const char* path) { return files.erase(path); }
  bool removeDir(const char* path) {
    const std::string prefix = std::string(path) + '/';
    for (auto i = files.begin(); i != files.end();) {
      if (i->first.starts_with(prefix))
        i = files.erase(i);
      else
        ++i;
    }
    directories.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) {
    if (renameFailures &&
        (exactRename ? std::string(to) == failRename : std::string(to).find(failRename) != std::string::npos)) {
      --renameFailures;
      return false;
    }
    auto i = files.find(from);
    if (i == files.end() || files.count(to)) return false;
    files[to] = std::move(i->second);
    files.erase(i);
    return true;
  }
  bool replaceFile(const char* from, const char* to) {
    const std::string backup = std::string(to) + ".bak";
    if (exists(backup.c_str())) {
      if (!exists(to) && !rename(backup.c_str(), to)) return false;
      if (exists(backup.c_str())) remove(backup.c_str());
    }
    if (!exists(from)) return false;
    const bool previous = exists(to);
    if (previous && !rename(to, backup.c_str())) return false;
    if (!rename(from, to)) {
      if (previous) rename(backup.c_str(), to);
      return false;
    }
    if (previous) remove(backup.c_str());
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file);
  bool openFileForWrite(const char*, const std::string& path, HalFile& file);
};
#define Storage HalStorage::getInstance()
class HalFile : public Print {
 public:
  TestRecord* record = nullptr;
  std::string path;
  size_t offset = 0;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (!record || (!Storage.failWrite.empty() && path.find(Storage.failWrite) != std::string::npos)) return 0;
    record->bytes.resize(std::max(record->bytes.size(), offset + n));
    memcpy(record->bytes.data() + offset, p, n);
    offset += n;
    ++record->time;
    return n;
  }
  int read(void* p, size_t n) {
    if (!record) return -1;
    Storage.maxReadRequested = std::max(Storage.maxReadRequested, n);
    n = std::min({n, Storage.maxRead, record->bytes.size() - std::min(offset, record->bytes.size())});
    memcpy(p, record->bytes.data() + offset, n);
    offset += n;
    Storage.readBytes += n;
    return int(n);
  }
  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  size_t size() const { return record ? record->bytes.size() : 0; }
  uint64_t fileSize64() const { return size(); }
  uint32_t modificationTime() const { return record ? record->time : 0; }
  bool seek(size_t n) {
    if (!record || n > size()) return false;
    offset = n;
    return true;
  }
  void flush() {}
  bool close() {
    record = nullptr;
    return Storage.failClose.empty() || path.find(Storage.failClose) == std::string::npos;
  }
};
inline bool HalStorage::openFileForRead(const char*, const std::string& path, HalFile& file) {
  auto i = files.find(path);
  if (i == files.end()) return false;
  file.record = &i->second;
  file.path = path;
  file.offset = 0;
  return true;
}
inline bool HalStorage::openFileForWrite(const char*, const std::string& path, HalFile& file) {
  file.record = &files[path];
  file.record->bytes.clear();
  file.path = path;
  file.offset = 0;
  return true;
}
