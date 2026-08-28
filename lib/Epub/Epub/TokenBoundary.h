#pragma once

#include <cstdint>

#include "hyphenation/HyphenationCommon.h"

// Shared line-break semantics for continuation tokens.  Focus Reading can
// split a source word into several visual tokens, so an attached token may
// still be breakable after an explicit visible hyphen.
namespace TokenBoundary {

constexpr bool allowsBreak(const bool continues, const bool noSpaceBefore) {
  return !continues || noSpaceBefore;
}

// Non-breaking hyphens keep the word intact; soft hyphens are handled by the
// hyphenator as an inserted visible '-'.
inline bool allowsBreakAfterExplicitHyphen(const uint32_t codepoint) {
  constexpr uint32_t NON_BREAKING_HYPHEN = 0x2011;
  return isExplicitHyphen(codepoint) && !isSoftHyphen(codepoint) && codepoint != NON_BREAKING_HYPHEN;
}

}  // namespace TokenBoundary
