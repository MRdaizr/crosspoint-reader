#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, TXT, or Markdown). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns the path-derived cache directory for a supported local book file,
// or an empty string for files that do not use reader caches.
std::string getBookCachePath(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
