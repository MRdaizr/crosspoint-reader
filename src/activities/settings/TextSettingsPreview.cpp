#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {
namespace {
CssTextAlign toCssAlign(const uint8_t align) {
  return align == CrossPointSettings::BOOK_STYLE ? CssTextAlign::Justify : static_cast<CssTextAlign>(align);
}

void relayout(PreviewLayout& layout, const GfxRenderer& renderer, const int fontId, const int textWidth) {
  layout.lines.clear();
  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;
  ParsedText parsed(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                    SETTINGS.focusReadingEnabled != 0, style);
  const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  std::string word;
  for (const char* p = text;; ++p) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }
  parsed.layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(textWidth),
                               [&layout](std::shared_ptr<TextBlock> line, uint32_t) {
                                 layout.lines.push_back(std::move(line));
                               });
}
}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, const int previewPadding, const int labelGap,
                   const int top, const int height, const char* familyName, const char* sizeName) {
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - previewPadding * 2;
  if (width <= 0 || height <= 0) return;
  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;
  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName ? familyName : "",
           sizeName ? sizeName : "");
  renderer.drawText(UI_10_FONT_ID, left, top + height - previewPadding - labelH, labelBuf);

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;
  const int lineH = renderer.getTextHeight(fontId);
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (lineH <= 0 || textWidth <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  // GfxRenderer exposes the base font line height; apply the reader's line
  // compression here to match ChapterHtmlSlimParser's layout calculation.
  const int lineAdvance = std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * compression + 0.5f));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0};
  if (!(key == layout.key)) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, I18N.get(StrId::STR_FONT_PREVIEW_TEXT), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  int y = top + previewPadding;
  const int bottom = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; ++paragraph) {
    for (const auto& line : layout.lines) {
      if (y + lineH > bottom) return;
      line->render(renderer, fontId, left + SETTINGS.screenMargin, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}
}  // namespace textsettings
