#pragma once
#include <HalStorage.h>

#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace serialization {
inline constexpr size_t MAX_STRING_BYTES = 4096;

// STL allocation is fatal with exceptions disabled. Check the contiguous heap
// before a bounded resize; the reserve accounts for allocator bookkeeping.
inline bool canAllocate(const size_t bytes) {
#ifdef ARDUINO
  constexpr size_t reserve = 512;
  return bytes <= SIZE_MAX - reserve && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + reserve;
#else
  (void)bytes;
  return true;
#endif
}
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
bool readPod(std::istream& is, T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    uint8_t raw = 0;
    if (!readPod(is, raw) || raw > 1) {
      value = false;
      return false;
    }
    value = raw != 0;
    return true;
  } else {
    if (is.read(reinterpret_cast<char*>(&value), sizeof(T))) return true;
    value = T{};
    return false;
  }
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    uint8_t raw = 0;
    if (!readPod(file, raw) || raw > 1) {
      value = false;
      return false;
    }
    value = raw != 0;
    return true;
  } else {
    if (static_cast<size_t>(file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T))) == sizeof(T)) return true;
    value = T{};
    return false;
  }
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline bool readString(std::istream& is, std::string& s, const size_t maxBytes = MAX_STRING_BYTES) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(is, len) || len > maxBytes || (len > s.capacity() && !canAllocate(size_t(len) + 1))) return false;
  s.resize(len);
  if (!len || is.read(s.data(), len)) return true;
  s.clear();
  return false;
}

inline bool readString(HalFile& file, std::string& s, const size_t maxBytes = MAX_STRING_BYTES) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(file, len) || len > maxBytes || file.available() < 0 || len > static_cast<size_t>(file.available()) ||
      (len > s.capacity() && !canAllocate(size_t(len) + 1)))
    return false;
  s.resize(len);
  if (!len || static_cast<size_t>(file.read(s.data(), len)) == len) return true;
  s.clear();
  return false;
}
}  // namespace serialization
