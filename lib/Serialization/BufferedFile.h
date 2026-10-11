#pragma once

#include <HalStorage.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace serialization {

// Sequential buffered wrappers over HalFile.  Book metadata generation reads
// and writes several files in an interleaved fashion; batching small POD and
// string operations avoids repeatedly evicting the SD sector cache.  If a
// buffer allocation fails, the wrappers transparently fall back to direct I/O.
class BufferedFileWriter {
 public:
  BufferedFileWriter(HalFile& file, const size_t capacity)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(capacity)), cap(buf ? capacity : 0), pos(file.position()) {}
  ~BufferedFileWriter() { flush(); }
  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

  void write(const void* src, const size_t len) {
    pos += len;
    const auto* p = static_cast<const uint8_t*>(src);
    if (fill + len > cap) flushBuffer();
    if (len >= cap) {
      okFlag &= file.write(p, len) == len;
      return;
    }
    uint8_t* const data = buf.get();
    memcpy(data + fill, p, len);
    fill += len;
  }

  size_t position() const { return pos; }

  bool flush() {
    flushBuffer();
    return okFlag;
  }

 private:
  void flushBuffer() {
    if (fill == 0) return;
    okFlag &= file.write(buf.get(), fill) == fill;
    fill = 0;
  }

  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  const size_t cap;
  size_t fill = 0;
  size_t pos;
  bool okFlag = true;
};

class BufferedFileReader {
 public:
  BufferedFileReader(HalFile& file, const size_t capacity)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(capacity)), cap(buf ? capacity : 0), bufStart(file.position()) {}
  BufferedFileReader(const BufferedFileReader&) = delete;
  BufferedFileReader& operator=(const BufferedFileReader&) = delete;

  size_t read(void* dst, size_t len) {
    auto* p = static_cast<uint8_t*>(dst);
    if (cap == 0) {
      const int n = file.read(p, len);
      const size_t got = n < 0 ? 0 : static_cast<size_t>(n);
      bufStart += got;
      return got;
    }
    size_t total = 0;
    while (len > 0) {
      if (off == fill) {
        bufStart += fill;
        off = 0;
        const int n = file.read(buf.get(), cap);
        fill = n < 0 ? 0 : static_cast<size_t>(n);
        if (fill == 0) break;
      }
      const size_t chunk = std::min(len, fill - off);
      const uint8_t* const data = buf.get();
      memcpy(p, data + off, chunk);
      p += chunk;
      off += chunk;
      len -= chunk;
      total += chunk;
    }
    return total;
  }

  size_t position() const { return bufStart + off; }

  size_t available() const { return static_cast<size_t>(std::max(0, file.available())) + fill - off; }

  bool seek(const size_t target) {
    if (cap != 0 && target >= bufStart && target < bufStart + fill) {
      off = target - bufStart;
      return true;
    }
    if (!file.seek(target)) return false;
    bufStart = target;
    fill = 0;
    off = 0;
    return true;
  }

 private:
  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  const size_t cap;
  size_t fill = 0;
  size_t off = 0;
  size_t bufStart;
};

template <typename T>
void writePod(BufferedFileWriter& out, const T& value) {
  out.write(&value, sizeof(T));
}

template <typename T>
bool readPod(BufferedFileReader& in, T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    uint8_t raw = 0;
    if (!readPod(in, raw) || raw > 1) {
      value = false;
      return false;
    }
    value = raw != 0;
    return true;
  } else {
    if (in.read(&value, sizeof(T)) == sizeof(T)) return true;
    value = T{};
    return false;
  }
}

inline void writeString(BufferedFileWriter& out, const std::string& s) {
  const uint32_t len = s.size();
  writePod(out, len);
  if (len > 0) out.write(s.data(), len);
}

inline bool readString(BufferedFileReader& in, std::string& s, const size_t maxBytes = MAX_STRING_BYTES) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(in, len) || len > maxBytes || len > in.available() ||
      (len > s.capacity() && !canAllocate(size_t(len) + 1)))
    return false;
  s.resize(len);
  if (!len || in.read(s.data(), len) == len) return true;
  s.clear();
  return false;
}

}  // namespace serialization
