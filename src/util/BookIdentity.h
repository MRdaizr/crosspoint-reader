#pragma once

#include <string>

// Stable identity helpers shared by reading statistics and book-side data.
// The content id follows KOReader's partial-content hash and therefore survives
// moving or renaming a book on the SD card.
namespace BookIdentity {

std::string normalizePath(const std::string& path);
std::string getFileExtensionLower(const std::string& path);
std::string calculateContentBookId(const std::string& path);
std::string resolveStableBookId(const std::string& path);
bool isLegacyBookId(const std::string& bookId);

}  // namespace BookIdentity
