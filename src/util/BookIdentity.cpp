#include "BookIdentity.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "KOReaderDocumentId.h"

namespace {
struct CachedIdentity {
  std::string path;
  size_t size = 0;
  std::string id;
};

std::vector<CachedIdentity>& cache() {
  static std::vector<CachedIdentity> value;
  return value;
}

size_t fileSize(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("BID", path, file)) return 0;
  return file.fileSize();
}
}  // namespace

namespace BookIdentity {

std::string normalizePath(const std::string& path) {
  if (path.empty()) return {};
  std::string normalized = FsHelpers::normalisePath(path);
  if (normalized.empty()) return {};
  if (normalized.front() != '/') normalized.insert(normalized.begin(), '/');
  return normalized;
}

std::string getFileExtensionLower(const std::string& path) {
  const std::string normalized = normalizePath(path);
  const size_t dot = normalized.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string extension = normalized.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

std::string calculateContentBookId(const std::string& path) {
  const std::string normalized = normalizePath(path);
  if (normalized.empty() || !Storage.exists(normalized.c_str())) return {};

  const size_t size = fileSize(normalized);
  auto& identities = cache();
  for (auto it = identities.begin(); it != identities.end(); ++it) {
    if (it->path == normalized && it->size == size) {
      if (it != identities.begin()) {
        CachedIdentity entry = *it;
        identities.erase(it);
        identities.insert(identities.begin(), std::move(entry));
      }
      return identities.front().id;
    }
  }

  const std::string id = KOReaderDocumentId::calculate(normalized);
  if (id.empty()) return {};
  identities.insert(identities.begin(), CachedIdentity{normalized, size, id});
  if (identities.size() > 24) identities.pop_back();
  return id;
}

std::string resolveStableBookId(const std::string& path) {
  const std::string normalized = normalizePath(path);
  if (normalized.empty()) return {};
  const std::string contentId = calculateContentBookId(normalized);
  return contentId.empty() ? "legacy:" + normalized : contentId;
}

bool isLegacyBookId(const std::string& bookId) { return bookId.rfind("legacy:", 0) == 0; }

}  // namespace BookIdentity
