#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  enum class ParseResult : uint8_t {
    Complete,
    Partial,
    Error,
  };

  enum class CacheStatus : uint8_t {
    Missing,
    Complete,
    Partial,
    Invalid,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // Version 9 adds a cache-status byte so a low-memory parse cannot be mistaken for a complete
  // stylesheet on the next open. Version 8 caches are intentionally rebuilt.
  static constexpr uint8_t CSS_CACHE_VERSION = 9;

  enum class CacheLoadResult : uint8_t {
    Complete,
    LowMemory,
    Invalid,
  };

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete unless bounded storage or malformed input stopped parsing
   */
  ParseResult loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty(); }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return rulesBySelector_.size(); }

  /**
   * Clear all loaded rules
   */
  void clear() {
    rulesBySelector_.clear();
    // std::unordered_map::clear() retains its bucket array. Release it too so
    // CSS parsing/hydration does not pin the rule table between section builds
    // or after a warm EPUB open.
    rulesBySelector_.rehash(0);
    selectorPoolBytes_ = 0;
    uniqueStyleCount_ = 0;
    ruleGrowthStopped_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /** Read the cache header without hydrating its rule map. */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete = true) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return Complete when loaded, LowMemory when loading should be retried,
   *         otherwise Invalid
   */
  CacheLoadResult loadFromCache();

 private:
  // Lookup key for a multi-piece selector. The pieces are hashed and compared
  // as if concatenated, so callers can look up composite keys without
  // materializing the concatenation in a scratch buffer. Constructed from a
  // braced list of any arity, e.g. `CompositeKey{tagName, ".", cls}` or
  // `CompositeKey{".", cls}`. The initializer_list's backing array lives for
  // the full expression, which covers the lifetime of the find() call.
  struct CompositeKey {
    std::initializer_list<std::string_view> pieces;
    CompositeKey(std::initializer_list<std::string_view> p) noexcept : pieces(p) {}
  };

  // ASCII-case-insensitive transparent hash/equal. Stored selectors and lookup
  // keys are compared without regard to case, so callers may insert and look up
  // using whatever case the CSS source or HTML element name happens to use.
  // Bodies live in CssParser.cpp so they can share the file-local asciiToLower.
  struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept;
    size_t operator()(const std::string& s) const noexcept;
    size_t operator()(CompositeKey k) const noexcept;
  };
  struct SvEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
    bool operator()(const std::string& a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, const std::string& b) const noexcept;
    bool operator()(const std::string& a, const std::string& b) const noexcept;
    bool operator()(CompositeKey a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, CompositeKey b) const noexcept;
  };

  // Storage: maps selector -> style properties. Hash/equal are case-insensitive.
  std::unordered_map<std::string, CssStyle, SvHash, SvEqual> rulesBySelector_;
  // Accounting limits for the map-backed parser. They mirror the bounded
  // selector/style pools used by the upstream implementation without changing
  // X4's lookup architecture.
  size_t selectorPoolBytes_ = 0;
  size_t uniqueStyleCount_ = 0;
  // Set while the current source stream had to drop rules because a bounded
  // parser pool or scratch buffer was exhausted. This is reset per stream.
  bool ruleGrowthStopped_ = false;

  std::string cachePath;

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
