#include "PageLink.h"

namespace {
template <typename T>
bool read(HalFile& file, T& value) {
  return file.read(&value, sizeof(value)) == sizeof(value);
}
template <typename T>
bool write(HalFile& file, const T& value) {
  return file.write(&value, sizeof(value)) == sizeof(value);
}
}  // namespace

bool PageLink::serialize(HalFile& file) const {
  if (!validGeometry()) {
    LOG_ERR("LNK", "Invalid geometry/target");
    return false;
  }
  const auto length = static_cast<uint16_t>(strlen(href.get()));
  return write(file, length) && file.write(href.get(), length) == length && write(file, identity) && write(file, x) &&
         write(file, y) && write(file, width) && write(file, height);
}

bool PageLink::deserialize(HalFile& file) {
  uint16_t length = 0;
  if (!read(file, length) || length == 0 || length > MAX_TARGET_BYTES) {
    LOG_ERR("LNK", "Invalid target length");
    return false;
  }
  auto target = makeUniqueNoThrow<char[]>(length + 1);
  if (!target) {
    LOG_ERR("LNK", "OOM: cached target");
    return false;
  }
  if (file.read(target.get(), length) != length || memchr(target.get(), 0, length)) {
    LOG_ERR("LNK", "Truncated/non-canonical target");
    return false;
  }
  target[length] = 0;
  href = std::move(target);
  if (!read(file, identity) || !read(file, x) || !read(file, y) || !read(file, width) || !read(file, height) ||
      !validGeometry()) {
    LOG_ERR("LNK", "Invalid cached geometry/target");
    return false;
  }
  return true;
}
