#include "NextBookFinder.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "CrossPointSettings.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

bool isSupportedBookFile(const std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) ||
         FsHelpers::hasTxtExtension(name) || FsHelpers::hasMarkdownExtension(name);
}
}  // namespace

std::vector<std::string> NextBookFinder::findNextBooks(const std::string& currentBookPath, const size_t maxCount) {
  std::vector<std::string> result;
  if (currentBookPath.empty() || maxCount == 0) return result;

  const std::string folder = FsHelpers::extractFolderPath(currentBookPath);
  const size_t lastSlash = currentBookPath.find_last_of('/');
  const std::string currentName = lastSlash == std::string::npos ? currentBookPath : currentBookPath.substr(lastSlash + 1);

  auto dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("NBF", "Cannot open folder: %s", folder.c_str());
    return result;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("NBF", "OOM: %u bytes", static_cast<unsigned>(NAME_BUFFER_SIZE));
    dir.close();
    return result;
  }

  result.reserve(maxCount + 1);
  const auto less = [](const std::string& left, const std::string& right) { return FsHelpers::naturalLess(left, right); };
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    if (!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') continue;
    if (!isSupportedBookFile(nameBuffer.get())) continue;

    std::string name{nameBuffer.get()};
    if (!FsHelpers::naturalLess(currentName, name)) continue;
    if (result.size() >= maxCount && !less(name, result.back())) continue;
    const auto position = std::lower_bound(result.begin(), result.end(), name, less);
    result.insert(position, std::move(name));
    if (result.size() > maxCount) result.pop_back();
  }
  dir.close();
  return result;
}
