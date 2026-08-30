#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {

struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

// These point sizes match the generated UI faces in fontIds.h.  Keep this
// table separate from the reader font-size enum: an SD family may provide a
// different set of reader sizes, while UI fallback must remain size-matched.
constexpr UiFontSize UI_FALLBACK_SIZES[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

constexpr uint32_t CJK_PROBES[] = {
    0x4E00,  // Han
    0x3042,  // Hiragana
    0x30A2,  // Katakana
    0xAC00,  // Hangul
};

}  // namespace

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        setupUiFallbacks(renderer);
        LOG_INF("SDFS", "Loaded SD card font family: %s (fontId=%d, size=%u)", SETTINGS.sdFontFamilyName,
                manager_.getLoadedFontId(), manager_.currentPointSize());
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_ERR("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_INF("SDFS", "No SD font family selected; dynamic UI text will use built-in fonts");
  }

  LOG_INF("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_ERR("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      setupUiFallbacks(renderer);
      LOG_INF("SDFS", "Loaded SD card font family: %s (fontId=%d, size=%u)", wantedFamily,
              manager_.getLoadedFontId(), manager_.currentPointSize());
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_ERR("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (family == nullptr) return;

  // Avoid loading several extra UI-size files for a Latin-only family. The
  // coverage query is RAM-only even though the loaded SD font keeps only a
  // page-sized glyph interval table resident.
  const int readerFontId = manager_.getLoadedFontId();
  const auto readerIt = renderer.getFontMap().find(readerFontId);
  if (readerIt == renderer.getFontMap().end()) return;

  bool hasCjk = false;
  for (const uint32_t cp : CJK_PROBES) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage; skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : UI_FALLBACK_SIZES) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
      LOG_DBG("SDFS", "UI fallback font %d -> SD %d (%u pt)", ui.fontId, sdFontId, ui.pointSize);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
  if (manager_.getLoadedFontId() == 0) return;
  manager_.unloadAll(renderer);
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}
