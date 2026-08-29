#pragma once

#include <cstdint>
#include <string>

// On-disk filename format for books downloaded from an OPDS server. Stored as
// a uint8_t in CrossPointSettings; cast to this enum at the call sites.
enum class OpdsFilenameFormat : uint8_t {
  AuthorTitle = 0,  // "Author - Title.epub" (default; matches legacy behaviour)
  TitleAuthor = 1,  // "Title - Author.epub"
  TitleOnly = 2,    // "Title.epub"
  Count = 3,
};

// Compose and sanitize the on-disk filename for a downloaded OPDS book.
// When the author is empty, every format collapses to the sanitized title.
std::string opdsBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format);
