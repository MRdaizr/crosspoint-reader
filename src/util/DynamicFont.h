#pragma once

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <string>

#include "SdCardFontSystem.h"

namespace DynamicFont {

inline bool isCjkCodepoint(uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||    // CJK punctuation
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
  const int sdFontId = sdFontSystem.currentFontId();
  if (containsCjk(text) && renderer.isSdCardFont(sdFontId)) {
    return sdFontId;
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
