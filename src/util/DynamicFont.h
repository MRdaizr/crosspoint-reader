#pragma once

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <string>

#include "SdCardFontSystem.h"

namespace DynamicFont {

inline bool isCjkCodepoint(uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||    // CJK punctuation
         (cp >= 0x3040 && cp <= 0x30FF) ||    // Hiragana and Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||    // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x9FFF) ||    // CJK unified ideographs
         (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||    // fullwidth forms
         (cp >= 0x20000 && cp <= 0x2FA1F);    // CJK extension planes
}

inline bool containsCjk(const char* text) {
  if (!text) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    if (isCjkCodepoint(utf8NextCodepoint(&p))) return true;
  }
  return false;
}

inline int fontForCjkText(const GfxRenderer& renderer, const char* text, int fallbackFontId) {
  if (!containsCjk(text)) return fallbackFontId;

  sdFontSystem.ensureLoaded(const_cast<GfxRenderer&>(renderer));
  const int sdFontId = sdFontSystem.currentFontId();
  if (renderer.isSdCardFont(sdFontId)) {
    return sdFontId;
  }

  static bool loggedMissingSdFont = false;
  if (!loggedMissingSdFont) {
    LOG_DBG("DFNT", "CJK text requested without a loaded SD font; falling back to built-in UI font");
    loggedMissingSdFont = true;
  }
  return fallbackFontId;
}

inline void prewarmIfSdFont(const GfxRenderer& renderer, int fontId, const std::string& text, uint8_t styleMask = 0x01) {
  if (text.empty() || !renderer.isSdCardFont(fontId)) return;
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->prewarmCache(fontId, text.c_str(), styleMask);
  }
}

}  // namespace DynamicFont
