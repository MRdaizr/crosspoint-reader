#pragma once

#include <string>

// Remove all per-book data after an explicit book deletion (EPUB/XTC/TXT/MD).
// For cache maintenance, use the selective invalidators below instead.
void clearBookCache(const std::string& path);
// Invalidate only regenerable data after replacement. Resume records and the
// global bookmark/statistics stores are retained, including interrupted saves.
bool invalidateBookCache(const std::string& path);
// Selective maintenance of one direct /.crosspoint/{epub,txt,xtc}_* directory.
// Broad roots, nested paths and traversal are rejected without mutation.
bool invalidateBookCacheDirectory(const std::string& path);
// The legacy TXT and unified layout caches use separate path hashes.
void relocateAdditionalTextCache(const std::string& oldPath, const std::string& newPath);

// Returns the path-derived cache directory for a supported local book file,
// or an empty string for files that do not use reader caches.
std::string getBookCachePath(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
