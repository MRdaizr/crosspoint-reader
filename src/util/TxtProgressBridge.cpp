#include "TxtProgressBridge.h"

#include <HalStorage.h>
#include <Logging.h>
#include <TxtBinary.h>
#include <TxtIo.h>

#include <cstring>

namespace {
constexpr uint32_t TXPR = 0x54585052, TXTI = 0x54585449, MARK_MAGIC = 0x31425054;
constexpr size_t MARK_SIZE = 48;
struct Snapshot {
  uint8_t data[12] = {};
  size_t size = 0;
  uint32_t time = 0;
  bool exists = false;
  uint64_t hash() const {
    return TxtBinary::hash(data, size, TxtBinary::HASH_SEED ^ size ^ (exists ? (1ULL << 63) : 0));
  }
};
bool recover(const std::string& cache) {
  const std::string path = cache + "/progress.bin";
  const std::string writerBackup = path + ".bak";
  const std::string backup = path + ".bridge.bak";
  // The normal reader writer's backup is more recent than an independently
  // retained bridge backup. Prefer it when recovering a legacy writer's gap.
  if (!Storage.exists(path.c_str()) && Storage.exists(writerBackup.c_str()))
    return Storage.rename(writerBackup.c_str(), path.c_str());
  // Independent backup additionally protects bridge rollback/publication.
  if (!Storage.exists(path.c_str()) && Storage.exists(backup.c_str()))
    return Storage.rename(backup.c_str(), path.c_str());
  return true;
}
bool snapshot(const std::string& cache, Snapshot& value) {
  if (!recover(cache)) return false;
  const std::string path = cache + "/progress.bin";
  value.exists = Storage.exists(path.c_str());
  if (!value.exists) return true;
  HalFile file;
  if (!Storage.openFileForRead("TPB", path, file)) return false;
  value.time = file.modificationTime();
  value.size = file.size();
  return value.size <= sizeof(value.data) && file.read(value.data, value.size) == int(value.size);
}
bool rawWrite(const std::string& path, const uint8_t* bytes, size_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("TPB", path, file) || file.write(bytes, size) != size) return false;
  file.flush();
  return file.close();
}
bool durableProgress(const std::string& cache, const uint8_t* bytes, size_t size,
                     const TxtToHtml::Callbacks* callbacks = nullptr) {
  if (TxtIo::cancelled(callbacks)) return false;
  Snapshot old;
  if (!snapshot(cache, old)) return false;
  if (old.exists && old.size == size && memcmp(old.data, bytes, size) == 0) return true;
  if (!Storage.exists(cache.c_str()) && !Storage.mkdir(cache.c_str())) return false;
  const std::string backup = cache + "/progress.bin.bridge.bak";
  if (old.exists && !rawWrite(backup + ".tmp", old.data, old.size)) return false;
  if (old.exists && !Storage.replaceFile((backup + ".tmp").c_str(), backup.c_str())) return false;
  // Same checked-close progress.bin.tmp publication protocol as ProgressFile;
  // additionally retain the bridge backup for cross-format rollback.
  const std::string path = cache + "/progress.bin";
  if (!rawWrite(path + ".tmp", bytes, size) || TxtIo::cancelled(callbacks) ||
      !Storage.replaceFile((path + ".tmp").c_str(), path.c_str())) {
    recover(cache);
    return false;
  }
  if (Storage.exists(backup.c_str())) Storage.remove(backup.c_str());
  return true;
}
bool restoreProgress(const std::string& cache, const Snapshot& old) {
  if (old.exists) return durableProgress(cache, old.data, old.size);
  const std::string path = cache + "/progress.bin";
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}
bool unifiedOffset(const Snapshot& unified, const TxtSourceMap& map, uint32_t& visible) {
  if (!unified.exists || unified.size != 10 || unified.data[0] != 0 || unified.data[1] != 0) return false;
  visible = TxtBinary::get32(unified.data + 6);
  return visible <= map.getVisibleCount();
}
bool marker(const std::string& cache, uint8_t* data) {
  const std::string path = cache + "/progress_text_sync.bin";
  if (!Storage.exists(path.c_str()) && Storage.exists((path + ".bak").c_str()))
    Storage.rename((path + ".bak").c_str(), path.c_str());
  HalFile file;
  return Storage.openFileForRead("TPB", path, file) && file.size() == MARK_SIZE &&
         file.read(data, MARK_SIZE) == int(MARK_SIZE) && TxtBinary::get32(data) == MARK_MAGIC &&
         TxtBinary::get32(data + 44) == TxtBinary::checksum(data, 44);
}
bool saveMarker(const std::string& unifiedPath, const std::string& legacyPath, const TxtSourceMap& map,
                const TxtToHtml::Callbacks* callbacks = nullptr) {
  Snapshot legacy, unified;
  if (!snapshot(legacyPath, legacy) || !snapshot(unifiedPath, unified)) return false;
  uint8_t data[MARK_SIZE] = {};
  TxtBinary::put32(data, MARK_MAGIC);
  TxtBinary::put64(data + 4, map.getSourceHash());
  TxtBinary::put64(data + 12, legacy.hash());
  TxtBinary::put64(data + 20, unified.hash());
  TxtBinary::put32(data + 28, legacy.time);
  TxtBinary::put32(data + 32, unified.time);
  TxtBinary::put32(data + 36, map.getSourceSize());
  TxtBinary::put32(data + 44, TxtBinary::checksum(data, 44));
  const std::string path = unifiedPath + "/progress_text_sync.bin";
  return rawWrite(path + ".tmp", data, sizeof(data)) && !TxtIo::cancelled(callbacks) &&
         Storage.replaceFile((path + ".tmp").c_str(), path.c_str());
}
bool cancelled(const TxtToHtml::Callbacks* cb) { return cb && cb->cancelled && cb->cancelled(cb->context); }
}  // namespace

TxtProgressBridge::TxtProgressBridge(const std::string& source, const std::string& unified, const TxtSourceMap& map)
    : sourcePath(source), unifiedCachePath(unified), sourceMap(map) {
  const auto slash = unified.find_last_of('/');
  legacyCachePath = unified.substr(0, slash) + "/txt_" + std::to_string(std::hash<std::string>{}(source));
}

bool TxtProgressBridge::legacyOffset(const uint8_t* data, size_t size, uint32_t& offset, uint32_t& page,
                                     bool allowIndex, const TxtToHtml::Callbacks* callbacks) const {
  if (size == 12 && TxtBinary::get32(data) == TXPR) {
    page = TxtBinary::get32(data + 4);
    offset = TxtBinary::get32(data + 8);
    return sourceMap.isSourceBoundary(offset);
  }
  // The old four-byte (page/count) format has no source offset. Only a fully
  // validated existing v5 index can bridge it; never build a guessed index here.
  if (size != 4 || !allowIndex) return false;
  page = uint32_t(data[0]) | (uint32_t(data[1]) << 8);
  return legacyIndexOffset(page, false, offset, page, callbacks);
}
bool TxtProgressBridge::legacyIndexOffset(uint32_t requestedPage, bool lastPage, uint32_t& offset, uint32_t& page,
                                          const TxtToHtml::Callbacks* callbacks) const {
  if (sourceMap.wasSourceChanged()) return false;
  HalFile index, source;
  if (!Storage.openFileForRead("TPB", legacyCachePath + "/index.bin", index) ||
      !Storage.openFileForRead("TPB", sourcePath, source))
    return false;
  if (source.size() != sourceMap.getSourceSize()) return false;
  if (!source.modificationTime() || !index.modificationTime() || source.modificationTime() > index.modificationTime())
    return false;
  uint8_t header[30];
  if (index.read(header, sizeof(header)) != int(sizeof(header)) || TxtBinary::get32(header) != TXTI || header[4] != 5 ||
      TxtBinary::get32(header + 5) != sourceMap.getSourceSize())
    return false;
  const uint32_t count = TxtBinary::get32(header + 26);
  page = lastPage && count ? count - 1 : requestedPage;
  if (!count || page >= count || count > uint64_t(sourceMap.getSourceSize()) + 1 ||
      index.fileSize64() != sizeof(header) + uint64_t(count) * 4)
    return false;
  uint8_t bytes[4];
  uint32_t previous = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (i % 128 == 0 && !TxtIo::pulse(callbacks, 0, sourceMap.getSourceSize())) return false;
    if (index.read(bytes, sizeof(bytes)) != int(sizeof(bytes))) return false;
    const uint32_t current = TxtBinary::get32(bytes);
    if (current > sourceMap.getSourceSize() || (i && current <= previous) || (!i && current != 0)) return false;
    if (current != sourceMap.getSourceSize()) {
      if (!source.seek(current)) return false;
      const int byte = source.read();
      if (byte < 0 || (byte & 0xC0) == 0x80) return false;
    }
    if (i == page) offset = current;
    previous = current;
  }
  return true;
}

TxtProgressBridge::Result TxtProgressBridge::migrateToUnified(const TxtToHtml::Callbacks* callbacks) {
  if (cancelled(callbacks)) return Result::Cancelled;
  if (!sourceMap.isReady()) return Result::IoError;
  Snapshot legacy, unified;
  if (!snapshot(legacyCachePath, legacy) || !snapshot(unifiedCachePath, unified)) return Result::IoError;
  if (!legacy.exists && !unified.exists) return Result::NoProgress;
  uint32_t oldOffset = 0, page = 0, oldVisible = 0, newVisible = 0;
  uint8_t mark[MARK_SIZE];
  const bool marked = marker(unifiedCachePath, mark);
  const bool sameSource = marked && TxtBinary::get64(mark + 4) == sourceMap.getSourceHash() &&
                          TxtBinary::get32(mark + 36) == sourceMap.getSourceSize();
  const bool oldValid = legacy.exists &&
                        legacyOffset(legacy.data, legacy.size, oldOffset, page,
                                     !sourceMap.wasSourceChanged() && (!marked || sameSource), callbacks) &&
                        sourceMap.sourceToVisible(oldOffset, oldVisible, callbacks);
  const bool newValid = unifiedOffset(unified, sourceMap, newVisible);
  if (cancelled(callbacks)) return Result::Cancelled;
  bool preferLegacy = oldValid && !newValid;
  if (oldValid && newValid) {
    if (sameSource) {
      const bool oldChanged = legacy.hash() != TxtBinary::get64(mark + 12);
      const bool newChanged = unified.hash() != TxtBinary::get64(mark + 20);
      if (oldChanged && !newChanged)
        preferLegacy = true;
      else if (!oldChanged && newChanged)
        preferLegacy = false;
      else if (!oldChanged && !newChanged)
        return Result::Ready;
      else if (oldVisible == newVisible)
        preferLegacy = false;
      else if (legacy.time && unified.time && legacy.time != unified.time)
        preferLegacy = legacy.time > unified.time;
      else
        return Result::NeedsCompatibility;
    } else if (marked)
      preferLegacy = true;  // Source changed: byte position can be remapped, old visible offset cannot.
    else if (oldVisible == newVisible)
      preferLegacy = false;
    else if (legacy.time && unified.time && legacy.time != unified.time)
      preferLegacy = legacy.time > unified.time;
    else
      return Result::NeedsCompatibility;
  }
  // Invalid legacy progress must not be silently ignored on first migration.
  if (legacy.exists && !oldValid && (!sameSource || legacy.hash() != TxtBinary::get64(mark + 12)))
    return Result::NeedsCompatibility;
  if (!preferLegacy && (!newValid || (marked && !sameSource))) return Result::NeedsCompatibility;
  if (cancelled(callbacks)) return Result::Cancelled;
  if (preferLegacy) {
    uint8_t bytes[10] = {};  // Old pagination is not the unified pagination.
    TxtBinary::put32(bytes + 6, oldVisible);
    if (!durableProgress(unifiedCachePath, bytes, sizeof(bytes), callbacks))
      return cancelled(callbacks) ? Result::Cancelled : Result::IoError;
    if (!saveMarker(unifiedCachePath, legacyCachePath, sourceMap, callbacks)) {
      if (!restoreProgress(unifiedCachePath, unified)) {
        LOG_ERR("TPB", "Failed to roll back unified progress");
      }
      return cancelled(callbacks) ? Result::Cancelled : Result::IoError;
    }
    return Result::Migrated;
  }
  if (mirrorUnifiedVisibleOffset(newVisible, page, callbacks)) return Result::Ready;
  return cancelled(callbacks) ? Result::Cancelled : Result::IoError;
}

bool TxtProgressBridge::mirrorUnifiedVisibleOffset(uint32_t visibleOffset, uint32_t legacyPageHint,
                                                   const TxtToHtml::Callbacks* callbacks) {
  Snapshot unified, legacy;
  uint32_t savedVisible, sourceOffset;
  if (!snapshot(unifiedCachePath, unified) || !snapshot(legacyCachePath, legacy) ||
      !unifiedOffset(unified, sourceMap, savedVisible) || savedVisible != visibleOffset ||
      !sourceMap.visibleToSource(visibleOffset, sourceOffset, callbacks))
    return false;
  if (sourceOffset == sourceMap.getSourceSize() && sourceOffset != 0) {
    uint32_t lastPageOffset;
    if (!legacyIndexOffset(0, true, lastPageOffset, legacyPageHint, callbacks)) return false;
  }
  if (sourceOffset == 0) legacyPageHint = 0;
  uint8_t bytes[12];
  TxtBinary::put32(bytes, TXPR);
  TxtBinary::put32(bytes + 4, legacyPageHint);
  TxtBinary::put32(bytes + 8, sourceOffset);
  if (!durableProgress(legacyCachePath, bytes, sizeof(bytes), callbacks)) return false;
  if (saveMarker(unifiedCachePath, legacyCachePath, sourceMap, callbacks)) return true;
  if (!restoreProgress(legacyCachePath, legacy)) {
    LOG_ERR("TPB", "Failed to roll back legacy progress");
  }
  return false;
}
