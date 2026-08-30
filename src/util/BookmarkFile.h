#pragma once

#include <string>
#include <vector>

#include "../BookmarkEntry.h"

// Centralized bookmark persistence.  The JSON representation remains exactly
// the existing one (including visible-text offsets), but callers no longer
// need to duplicate path/directory and parse error handling.
namespace BookmarkFile {
bool load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks);
bool save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks);
}  // namespace BookmarkFile
