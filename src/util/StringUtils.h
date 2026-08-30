#pragma once

#include <cctype>
#include <string>

namespace StringUtils {

// Case-insensitive ASCII comparison used by streamed StarDict indexes and
// dictionary-folder sorting. Keep it inline because the binary-search path
// invokes it for every sampled comparison.
inline int asciiCaseCmp(const char* a, const char* b) {
  while (*a && *b) {
    const int diff = std::tolower(static_cast<unsigned char>(*a)) -
                     std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    ++a;
    ++b;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

}  // namespace StringUtils
