#include "TxtSourceMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "TxtBinary.h"
#include "TxtIo.h"

namespace {
constexpr uint32_t MANIFEST_MAGIC = 0x31465254;  // TRF1
constexpr uint32_t MAP_MAGIC = 0x31504D54;       // TMP1
constexpr uint32_t VERSION = 1;
constexpr size_t MANIFEST_SIZE = 64, HEADER_SIZE = 32, RECORD_SIZE = 24;
constexpr uint32_t STRIDE = 4096;
bool cancelled(const TxtToHtml::Callbacks* cb) { return cb && cb->cancelled && cb->cancelled(cb->context); }
int readSource(void* context, uint8_t* buffer, size_t size) {
  return static_cast<HalFile*>(context)->read(buffer, size);
}
bool fingerprint(const std::string& path, uint32_t& size, uint64_t& hash, const TxtToHtml::Callbacks* cb = nullptr) {
  HalFile file;
  if (!Storage.openFileForRead("TXM", path, file) || file.fileSize64() > UINT32_MAX) return false;
  size = file.size();
  hash = TxtBinary::HASH_SEED;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(1024);
  if (!buffer) {
    LOG_ERR("TXM", "OOM fingerprinting text");
    return false;
  }
  uint32_t offset = 0;
  while (offset < size) {
    if (cancelled(cb)) return false;
    const int n = file.read(buffer.get(), std::min<uint32_t>(1024, size - offset));
    if (n <= 0) return false;
    hash = TxtBinary::hash(buffer.get(), n, hash);
    offset += n;
    // Also yields the render task during validation/revalidation, without
    // pretending this validation pass is conversion progress.
    if (!TxtIo::pulse(cb, 0, size)) return false;
  }
  return !cancelled(cb) && file.size() == size;
}
bool readExact(const std::string& path, uint8_t* data, size_t length) {
  HalFile file;
  return Storage.openFileForRead("TXM", path, file) && file.size() == length && file.read(data, length) == int(length);
}
bool writeExact(const std::string& path, const uint8_t* data, size_t length) {
  HalFile file;
  if (!Storage.openFileForWrite("TXM", path, file) || file.write(data, length) != length) return false;
  file.flush();
  return file.close();  // Publication requires the FAT directory/size update too.
}
bool validManifest(const uint8_t* data) {
  return TxtBinary::get32(data) == MANIFEST_MAGIC && TxtBinary::get32(data + 4) == VERSION &&
         TxtBinary::get32(data + 8) < 2 && TxtBinary::get32(data + 60) == TxtBinary::checksum(data, 60);
}
std::string generationPath(const std::string& cache, uint32_t slot, const char* extension) {
  return cache + (slot ? "/text1" : "/text0") + extension;
}
struct MapWriter {
  HalFile* file;
  uint32_t last = 0, count = 0;
  bool write(const TxtToHtml::State& state) {
    uint8_t record[RECORD_SIZE] = {};
    TxtBinary::put32(record, state.source);
    TxtBinary::put32(record + 4, state.visible);
    TxtBinary::put32(record + 8, state.spaces);
    TxtBinary::put32(record + 12, state.spaceStart);
    TxtBinary::put32(record + 16, state.lineStart ? 1 : 0);
    TxtBinary::put32(record + 20, TxtBinary::checksum(record, 20));
    if (file->write(record, sizeof(record)) != sizeof(record)) return false;
    last = state.source;
    ++count;
    return true;
  }
  static bool checkpoint(void* context, const TxtToHtml::State& state) {
    auto& self = *static_cast<MapWriter*>(context);
    return state.source - self.last < STRIDE || self.write(state);
  }
};
// Keep publication headers and handles off the small ESP32 render-task stack.
// Fixed-size workspace, independent of source length (in addition to IO buffers).
struct Preparation {
  uint8_t manifest[MANIFEST_SIZE] = {};
  uint8_t header[HEADER_SIZE] = {};
  std::string manifestPath;
  HalFile source, html, map;
  TxtToHtml::State state;
};
class Discard : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t size) override { return size; }
};
bool readState(HalFile& map, uint32_t index, TxtToHtml::State& state) {
  uint8_t record[RECORD_SIZE];
  if (!map.seek(HEADER_SIZE + uint64_t(index) * RECORD_SIZE) ||
      map.read(record, sizeof(record)) != int(sizeof(record)) ||
      TxtBinary::get32(record + 20) != TxtBinary::checksum(record, 20))
    return false;
  state.source = TxtBinary::get32(record);
  state.visible = TxtBinary::get32(record + 4);
  state.spaces = TxtBinary::get32(record + 8);
  state.spaceStart = TxtBinary::get32(record + 12);
  state.lineStart = TxtBinary::get32(record + 16) != 0;
  return state.visible >= 1 && state.spaces <= state.source && (!state.spaces || state.spaceStart < state.source);
}
struct Lookup {
  const std::string* path;
  uint32_t target, result = 0;
  bool reverse, found = false, failed = false;
  const TxtToHtml::Callbacks* callbacks;
  // A deferred space run can straddle checkpoints and contain ignored CRs.
  // Read its source interval, rather than assuming contiguous source bytes.
  bool scanSpaces(const TxtToHtml::Event& event) {
    HalFile file;
    if (!Storage.openFileForRead("TXM", *path, file) || !file.seek(event.sourceStart)) return false;
    auto buffer = makeUniqueNoThrow<uint8_t[]>(1024);
    if (!buffer) {
      LOG_ERR("TXM", "OOM replaying spaces");
      return false;
    }
    uint32_t offset = event.sourceStart, count = 0;
    while (offset < event.sourceEnd) {
      if (!TxtIo::pulse(callbacks, 0, 0)) return false;
      const int n = file.read(buffer.get(), std::min<uint32_t>(1024, event.sourceEnd - offset));
      if (n <= 0) return false;
      for (int i = 0; i < n; ++i, ++offset) {
        if (!reverse && offset >= target) {
          result = event.visibleStart + count;
          return true;
        }
        if (buffer[i] == ' ') {
          if (reverse && count == target - event.visibleStart) {
            result = offset;
            return true;
          }
          ++count;
        }
      }
    }
    result = event.visibleStart + count;
    return !reverse;
  }
  static bool event(void* context, const TxtToHtml::Event& event) {
    auto& self = *static_cast<Lookup*>(context);
    if (self.reverse) {
      if (self.target >= event.visibleStart + event.count) return true;
      self.result = event.sourceStart;
      if (event.spaces && !self.scanSpaces(event)) self.failed = true;
    } else {
      if (self.target >= event.sourceEnd && event.sourceEnd != event.sourceStart) return true;
      self.result = event.visibleStart;
      if (event.spaces && self.target > event.sourceStart && !self.scanSpaces(event)) self.failed = true;
    }
    self.found = true;
    return false;
  }
};
}  // namespace

bool TxtSourceMap::prepare(bool buildIfMissing, const TxtToHtml::Callbacks* callbacks) {
  ready = false;
  auto workspace = makeUniqueNoThrow<Preparation>();
  if (!workspace) {
    LOG_ERR("TXM", "OOM preparing publication");
    return false;
  }
  auto& work = *workspace;
  uint32_t size;
  uint64_t digest;
  if (!fingerprint(sourcePath, size, digest, callbacks)) {
    LOG_ERR("TXM", "Cannot fingerprint source");
    return false;
  }
  work.manifestPath = cachePath + "/text.manifest";
  const auto& manifestPath = work.manifestPath;
  // replaceFile retains .bak between FAT renames. Recover before inspecting it.
  if (!Storage.exists(manifestPath.c_str()) && Storage.exists((manifestPath + ".bak").c_str()) &&
      !Storage.rename((manifestPath + ".bak").c_str(), manifestPath.c_str()))
    return false;
  auto* manifest = work.manifest;
  const bool previous = readExact(manifestPath, manifest, MANIFEST_SIZE) && validManifest(manifest);
  // Sticky evidence survives reopening the regenerated cache. A v5 legacy
  // index contains no source digest, so it cannot authenticate this update.
  sourceChanged = previous && (TxtBinary::get32(manifest + 56) != 0 || TxtBinary::get32(manifest + 12) != size ||
                               TxtBinary::get64(manifest + 16) != digest);
  const uint32_t previousSlot = previous ? TxtBinary::get32(manifest + 8) : 1;
  if (previous && TxtBinary::get32(manifest + 12) == size && TxtBinary::get64(manifest + 16) == digest) {
    uint32_t hSize, mSize;
    uint64_t hHash, mHash;
    htmlPath = generationPath(cachePath, previousSlot, ".html");
    mapPath = generationPath(cachePath, previousSlot, ".map");
    if (fingerprint(htmlPath, hSize, hHash, callbacks) && fingerprint(mapPath, mSize, mHash, callbacks) &&
        hSize == TxtBinary::get32(manifest + 24) && hHash == TxtBinary::get64(manifest + 28) &&
        mSize == TxtBinary::get32(manifest + 36) && mHash == TxtBinary::get64(manifest + 40)) {
      sourceSize = size;
      sourceHash = digest;
      htmlSize = hSize;
      visibleCount = TxtBinary::get32(manifest + 48);
      recordCount = TxtBinary::get32(manifest + 52);
      ready = visibleCount >= 2 && recordCount && uint64_t(HEADER_SIZE) + uint64_t(recordCount) * RECORD_SIZE == mSize;
      if (ready) return true;
    }
  }
  if (!buildIfMissing || cancelled(callbacks)) return false;
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) return false;
  const uint32_t slot = previousSlot ^ 1;
  htmlPath = generationPath(cachePath, slot, ".html");
  mapPath = generationPath(cachePath, slot, ".map");
  uint32_t records = 0, visible = 0;
  {
    auto& source = work.source;
    auto& html = work.html;
    auto& map = work.map;
    if (!Storage.openFileForRead("TXM", sourcePath, source) || source.size() != size ||
        !Storage.openFileForWrite("TXM", htmlPath + ".tmp", html) ||
        !Storage.openFileForWrite("TXM", mapPath + ".tmp", map))
      return false;
    auto* header = work.header;
    if (map.write(header, HEADER_SIZE) != HEADER_SIZE) return false;
    MapWriter writer{&map};
    auto& state = work.state;
    if (!writer.write(state)) return false;
    const TxtToHtml::Observer observer{&writer, nullptr, MapWriter::checkpoint};
    if (!TxtToHtml::convert(sourcePath, &source, readSource, html, size, callbacks, &observer, state)) {
      LOG_ERR("TXM", "Text conversion cancelled or failed");
      return false;
    }
    visible = state.visible;
    records = writer.count;
    TxtBinary::put32(header, MAP_MAGIC);
    TxtBinary::put32(header + 4, VERSION);
    TxtBinary::put32(header + 8, size);
    TxtBinary::put64(header + 12, digest);
    TxtBinary::put32(header + 20, records);
    TxtBinary::put32(header + 24, visible);
    TxtBinary::put32(header + 28, TxtBinary::checksum(header, 28));
    if (!map.seek(0) || map.write(header, HEADER_SIZE) != HEADER_SIZE) return false;
    html.flush();
    map.flush();
    if (!html.close() || !map.close()) {
      LOG_ERR("TXM", "Cannot close converted generation");
      return false;
    }
    source.close();  // Workspace-owned handle, release before source revalidation.
  }
  uint32_t currentSize, hSize, mSize;
  uint64_t currentHash, hHash, mHash;
  if (!fingerprint(sourcePath, currentSize, currentHash, callbacks) || currentSize != size || currentHash != digest ||
      !fingerprint(htmlPath + ".tmp", hSize, hHash, callbacks) ||
      !fingerprint(mapPath + ".tmp", mSize, mHash, callbacks) || cancelled(callbacks))
    return false;
  if (!Storage.replaceFile((htmlPath + ".tmp").c_str(), htmlPath.c_str()) ||
      !Storage.replaceFile((mapPath + ".tmp").c_str(), mapPath.c_str()))
    return false;
  memset(manifest, 0, MANIFEST_SIZE);
  TxtBinary::put32(manifest, MANIFEST_MAGIC);
  TxtBinary::put32(manifest + 4, VERSION);
  TxtBinary::put32(manifest + 8, slot);
  TxtBinary::put32(manifest + 12, size);
  TxtBinary::put64(manifest + 16, digest);
  TxtBinary::put32(manifest + 24, hSize);
  TxtBinary::put64(manifest + 28, hHash);
  TxtBinary::put32(manifest + 36, mSize);
  TxtBinary::put64(manifest + 40, mHash);
  TxtBinary::put32(manifest + 48, visible);
  TxtBinary::put32(manifest + 52, records);
  TxtBinary::put32(manifest + 56, sourceChanged ? 1 : 0);
  TxtBinary::put32(manifest + 60, TxtBinary::checksum(manifest, 60));
  if (!writeExact(manifestPath + ".tmp", manifest, MANIFEST_SIZE) || cancelled(callbacks)) return false;
  // Invalidate BEFORE switching the generation, including on interrupted publication.
  // Never remove the book directory, progress.bin, bookmarks, or companion cover.
  const std::string sections = cachePath + "/sections";
  if (Storage.exists(sections.c_str()) && !Storage.removeDir(sections.c_str())) return false;
  if (!Storage.replaceFile((manifestPath + ".tmp").c_str(), manifestPath.c_str())) return false;
  sourceSize = size;
  sourceHash = digest;
  htmlSize = hSize;
  visibleCount = visible;
  recordCount = records;
  ready = true;
  return true;
}

bool TxtSourceMap::lookup(uint32_t target, bool reverse, uint32_t& result,
                          const TxtToHtml::Callbacks* callbacks) const {
  if (!ready || TxtIo::cancelled(callbacks) || (reverse ? target > visibleCount : target > sourceSize)) return false;
  if (reverse && target == 0) {
    result = 0;
    return true;
  }
  if (reverse && target >= visibleCount - 1) {
    result = sourceSize;
    return true;
  }
  if (!reverse && target == sourceSize) {
    result = visibleCount - 1;
    return true;
  }
  HalFile map, source;
  if (!Storage.openFileForRead("TXM", mapPath, map) || !Storage.openFileForRead("TXM", sourcePath, source) ||
      source.size() != sourceSize)
    return false;
  TxtToHtml::State state;
  uint32_t low = 0, high = recordCount;
  while (low + 1 < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (!readState(map, mid, state)) return false;
    if ((reverse ? state.visible : state.source) <= target)
      low = mid;
    else
      high = mid;
  }
  if (!readState(map, low, state) || state.source > sourceSize || state.visible > visibleCount ||
      !source.seek(state.source))
    return false;
  Lookup query{&sourcePath, target, 0, reverse, false, false, callbacks};
  const TxtToHtml::Observer observer{&query, Lookup::event, nullptr};
  Discard sink;
  if (!TxtToHtml::convert(sourcePath, &source, readSource, sink, sourceSize, callbacks, &observer, state, true) ||
      !query.found || query.failed)
    return false;
  result = query.result;
  return true;
}
bool TxtSourceMap::sourceToVisible(uint32_t source, uint32_t& visible, const TxtToHtml::Callbacks* callbacks) const {
  return lookup(source, false, visible, callbacks);
}
bool TxtSourceMap::visibleToSource(uint32_t visible, uint32_t& source, const TxtToHtml::Callbacks* callbacks) const {
  return lookup(visible, true, source, callbacks);
}
bool TxtSourceMap::isSourceBoundary(uint32_t offset) const {
  if (!ready || offset > sourceSize) return false;
  if (offset == sourceSize) return true;
  HalFile source;
  if (!Storage.openFileForRead("TXM", sourcePath, source) || !source.seek(offset)) return false;
  const int byte = source.read();
  return byte >= 0 && (byte & 0xC0) != 0x80;
}
