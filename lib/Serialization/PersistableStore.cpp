#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr size_t MAX_JSON_BYTES = 64 * 1024;
// Serialize publication and recovery, including callers such as BookmarkFile
// which do not inherit a storeMutex. No complete JSON string is kept in RAM.
std::mutex jsonIoMutex;

class JsonFileWriter {
 public:
  explicit JsonFileWriter(HalFile& file) : file(file) {}
  size_t write(const uint8_t byte) { return write(&byte, 1); }
  size_t write(const uint8_t* data, const size_t size) {
    if (failed) return 0;
    size_t done = 0;
    while (done < size) {
      if (used == sizeof(buffer) && !flush()) return done;
      const size_t count = std::min(size - done, sizeof(buffer) - used);
      memcpy(buffer + used, data + done, count);
      used += count;
      done += count;
    }
    return done;
  }
  bool flush() {
    if (failed) return false;
    if (used && file.write(buffer, used) != used) failed = true;
    used = 0;
    return !failed;
  }

 private:
  HalFile& file;
  uint8_t buffer[128];
  size_t used = 0;
  bool failed = false;
};

class JsonFileReader {
 public:
  JsonFileReader(HalFile& file, const size_t size) : file(file), remaining(size) {}
  int read() {
    if (offset == used) {
      if (!remaining || failed) return -1;
      const size_t wanted = std::min(remaining, sizeof(buffer));
      const int got = file.read(buffer, wanted);
      if (got != static_cast<int>(wanted)) {
        failed = true;
        return -1;
      }
      remaining -= wanted;
      used = wanted;
      offset = 0;
    }
    return buffer[offset++];
  }
  size_t readBytes(char* out, const size_t size) {
    size_t done = 0;
    for (; done < size; ++done) {
      const int byte = read();
      if (byte < 0) break;
      out[done] = static_cast<char>(byte);
    }
    return done;
  }
  bool finish() {
    int byte;
    while ((byte = read()) >= 0)
      if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') return false;
    return !failed;
  }

 private:
  HalFile& file;
  uint8_t buffer[128];
  size_t remaining, offset = 0, used = 0;
  bool failed = false;
};

bool readJson(const char* path, JsonDocument& doc) {
  doc.clear();
  HalFile file;
  if (!Storage.openFileForRead("PERSIST", path, file) || file.isDirectory()) return false;
  const uint64_t size = file.fileSize64();
  if (!size || size > MAX_JSON_BYTES) return false;
  JsonFileReader reader(file, static_cast<size_t>(size));
  const auto error = deserializeJson(doc, reader);
  if (error || !reader.finish()) {
    LOG_ERR("PERSIST", "Invalid or incomplete JSON in %s", path);
    doc.clear();
    return false;
  }
  return true;
}
}  // namespace

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  if (!path || !*path || doc.overflowed()) return false;
  std::lock_guard<std::mutex> lock(jsonIoMutex);
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  const size_t expected = measureJson(doc);
  if (!expected || expected > MAX_JSON_BYTES) return false;
  const std::string tmp = std::string(path) + ".tmp";
  bool written = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PERSIST", tmp, file)) return false;
    JsonFileWriter writer(file);
    const size_t count = serializeJson(doc, writer);
    const bool complete = writer.flush() && count == expected;
    file.flush();
    const bool closed = file.close();
    written = complete && closed;
  }
  if (!written || !Storage.replaceFile(tmp.c_str(), path)) {
    Storage.remove(tmp.c_str());
    LOG_ERR("PERSIST", "Failed to publish %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  if (!path || !*path) return false;
  std::lock_guard<std::mutex> lock(jsonIoMutex);
  const std::string backup = std::string(path) + ".bak";
  if (!Storage.exists(path) && Storage.exists(backup.c_str())) {
    if (!Storage.rename(backup.c_str(), path)) return false;
  }
  if (readJson(path, doc)) return true;
  // A backup may survive a power cut after publication. Prefer the canonical
  // file when valid, but retain the older complete snapshot if it is damaged.
  return Storage.exists(backup.c_str()) && readJson(backup.c_str(), doc);
}

bool PersistableStoreBase::hasDocFile(const char* path) {
  if (!path || !*path) return false;
  std::lock_guard<std::mutex> lock(jsonIoMutex);
  return Storage.exists(path) || Storage.exists((std::string(path) + ".bak").c_str());
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  return pass;
}
