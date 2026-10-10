#pragma once

#include <Print.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

class HalFile : public Print {
 public:
  std::string* contents = nullptr;
  size_t offset = 0;
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (!contents) return 0;
    if (offset + count > contents->size()) contents->resize(offset + count);
    memcpy(contents->data() + offset, bytes, count);
    offset += count;
    return count;
  }
  int read(void* bytes, size_t count) {
    if (!contents) return 0;
    count = std::min(count, contents->size() - std::min(offset, contents->size()));
    memcpy(bytes, contents->data() + offset, count);
    offset += count;
    return static_cast<int>(count);
  }
  int available() const {
    return contents ? static_cast<int>(contents->size() - std::min(offset, contents->size())) : 0;
  }
  size_t position() const { return offset; }
  bool seek(size_t position) {
    offset = position;
    return contents != nullptr;
  }
  bool close() {
    contents = nullptr;
    return true;
  }
  explicit operator bool() const { return contents != nullptr; }
};

class HalStorage {
 public:
  std::map<std::string, std::string> files;
  unsigned reads = 0;
  unsigned writes = 0;
  unsigned removals = 0;
  unsigned renames = 0;
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  void reset() {
    files.clear();
    reads = writes = removals = renames = 0;
  }
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    ++reads;
    const auto found = files.find(path);
    if (found == files.end()) return false;
    file.contents = &found->second;
    file.offset = 0;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    ++writes;
    file.contents = &files[path];
    file.contents->clear();
    file.offset = 0;
    return true;
  }
  bool remove(const char* path) {
    ++removals;
    return files.erase(path) != 0;
  }
  bool rename(const char* from, const char* to) {
    ++renames;
    const auto found = files.find(from);
    if (found == files.end()) return false;
    files[to] = std::move(found->second);
    files.erase(found);
    return true;
  }
};

#define Storage HalStorage::getInstance()
