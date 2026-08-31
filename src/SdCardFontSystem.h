#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Release resident glyph buffers while retaining the selected family.
  /// The next ensureLoaded()/font lookup reloads them on demand.
  void releaseLoadedFont(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + physical point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Returns the currently loaded SD font ID, or 0 if no SD font is active.
  int currentFontId() const { return manager_.getLoadedFontId(); }

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }
  SdCardFontRegistry& registry() { return registry_; }

 private:
  // Keep reader SD fonts usable when a glyph is absent from the selected
  // family.  The retained NotoSerif14 face is the final built-in fallback in
  // the slim profile (and is also safe for full builds).
  void setupReaderFallback(GfxRenderer& renderer);

  // Register size-matched SD fonts as CJK fallbacks for the built-in UI faces.
  // The reader-size font remains the selected font for EPUB content; these
  // additional files are only used when a UI string contains a glyph missing
  // from the built-in face.
  void setupUiFallbacks(GfxRenderer& renderer);

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
