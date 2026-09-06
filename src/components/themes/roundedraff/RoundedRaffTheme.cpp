#include "RoundedRaffTheme.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
constexpr int kCoverRadius = 18;
constexpr int kMenuRadius = 30;
constexpr int kBottomRadius = 15;
constexpr int kRowRadius = 20;
constexpr int kInteractiveInsetX = 20;
constexpr int kSelectableRowGap = 6;

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  const int barW = RoundedRaffMetrics::values.scrollBarWidth;
  const int barX = rect.x + rect.width - RoundedRaffMetrics::values.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(10, (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
  const int thumbY = barY + (clampedStart * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

}  // namespace
int coverWidth = 0;

int RoundedRaffTheme::titleFontId() const { return UI_12_FONT_ID; }

int RoundedRaffTheme::subtitleFontId() const { return SMALL_FONT_ID; }

int RoundedRaffTheme::guideFontId() const { return SMALL_FONT_ID; }

ListRowLayout RoundedRaffTheme::getListRowLayout(const GfxRenderer& renderer, int contentHeight, int titleFontId,
                                                 bool hasSubtitle) const {
  const int fontId = titleFontId != 0 ? titleFontId : this->titleFontId();
  const int titleLineHeight = renderer.getLineHeight(fontId);
  const int rowHeight = hasSubtitle ? 10 + titleLineHeight + 4 + renderer.getLineHeight(subtitleFontId()) + 10
                                    : RoundedRaffMetrics::values.listRowHeight;
  const int titleOffsetY = hasSubtitle ? 10 : (rowHeight - titleLineHeight) / 2;
  const int rowStep = rowHeight + kSelectableRowGap;
  return {rowHeight, rowStep, titleOffsetY, titleLineHeight, std::max(1, contentHeight / rowStep)};
}

void RoundedRaffTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                                  const char* subtitle) const {
  (void)subtitle;
  // Home screen header is custom-rendered in drawRecentBookCover.
  if (title == nullptr) {
    return;
  }
  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int titleX = rect.x + sidePadding;
  const int titleY = rect.y + 14;

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryIconX = rect.x + rect.width - sidePadding - RoundedRaffMetrics::values.batteryWidth;

  // Reserve space for the widest possible percentage text to avoid title/battery overlap
  int batteryGroupLeftX = batteryIconX;
  if (showBatteryPercentage) {
    // Clear a fixed-width area for the battery percentage to avoid ghosting when digit count changes (e.g. 100% -> 99%)
    const int maxTextWidth = renderer.getTextWidth(subtitleFontId(), "100%");
    batteryGroupLeftX -= maxTextWidth + batteryPercentSpacing;

    const int clearW = maxTextWidth + batteryPercentSpacing + RoundedRaffMetrics::values.batteryWidth;
    const int clearH =
        std::max(renderer.getTextHeight(subtitleFontId()), RoundedRaffMetrics::values.batteryHeight + 8);
    renderer.fillRect(batteryIconX - maxTextWidth - batteryPercentSpacing, rect.y + 14, clearW, clearH, false);
  }

  const int maxTitleWidth = std::max(0, batteryGroupLeftX - 20 - titleX);
  const int headerFontId = DynamicFont::fontForCjkText(renderer, title, titleFontId());
  const auto headerStyle = renderer.isSdCardFont(headerFontId) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
  std::string prewarmText = title;
  prewarmText += "\xe2\x80\xa6";
  DynamicFont::prewarmIfSdFont(renderer, headerFontId, prewarmText);
  auto headerTitle = renderer.truncatedText(headerFontId, title, maxTitleWidth, headerStyle);
  renderer.drawText(headerFontId, titleX, titleY, headerTitle.c_str(), true, headerStyle);
  const Rect batteryRect{batteryIconX, rect.y + 14, RoundedRaffMetrics::values.batteryWidth,
                         RoundedRaffMetrics::values.batteryHeight};
  const uint16_t percentage = powerManager.getBatteryPercentage();
  if (showBatteryPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(subtitleFontId(), percentageText.c_str());
    renderer.drawText(subtitleFontId(), batteryRect.x - textWidth - batteryPercentSpacing, batteryRect.y,
                      percentageText.c_str());
  }
  const int batteryY = batteryRect.y + 6;
  const Rect iconRect{batteryRect.x, batteryY, batteryRect.width, batteryRect.height};
  drawBatteryOutline(renderer, batteryRect.x, batteryY, batteryRect.width, batteryRect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void RoundedRaffTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                  bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int slotWidth = rect.width / static_cast<int>(tabs.size());
  const int tabY = rect.y + 4;
  const int tabHeight = rect.height - 12;

  for (size_t i = 0; i < tabs.size(); i++) {
    const int slotX = rect.x + static_cast<int>(i) * slotWidth;
    const int tabX = slotX + 4;
    const int tabWidth = slotWidth - 8;
    const auto& tab = tabs[i];

    if (tab.selected) {
      renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, 18, selected ? Color::Black : Color::DarkGray);
    }

    const int textWidth = renderer.getTextWidth(titleFontId(), tab.label, EpdFontFamily::BOLD);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(titleFontId())) / 2;
    renderer.drawText(titleFontId(), textX, textY, tab.label, !(tab.selected), EpdFontFamily::BOLD);
  }

  // Full-width divider between tabs and setting rows.
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void RoundedRaffTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = rect.width - 2 * RoundedRaffMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = RoundedRaffMetrics::values.homeCoverHeight * 0.6;
  }
  const int imgY = tileY + (tileHeight - RoundedRaffMetrics::values.homeCoverHeight) / 2;
  const int tileX = RoundedRaffMetrics::values.contentSidePadding;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(coverPath, RoundedRaffMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                RoundedRaffMetrics::values.homeCoverHeight);
            renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                                   RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                                   Color::LightGray);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRoundedRect(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                               RoundedRaffMetrics::values.homeCoverHeight, 1, kCoverRadius, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + (tileWidth - coverWidth) / 2, imgY + (RoundedRaffMetrics::values.homeCoverHeight / 3),
                          coverWidth, 2 * RoundedRaffMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + (tileWidth - coverWidth) / 2 + 24, imgY + 24, 32, 32);
        renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                               RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                               Color::LightGray);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    renderer.fillRoundedRect(tileX, tileY, tileWidth, imgY - tileY, kRowRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, imgY, (tileWidth - coverWidth) / 2, RoundedRaffMetrics::values.homeCoverHeight,
                            Color::LightGray);
    renderer.fillRectDither(tileX + (tileWidth + coverWidth) / 2, imgY, (tileWidth - coverWidth) / 2,
                            RoundedRaffMetrics::values.homeCoverHeight, Color::LightGray);
    renderer.fillRoundedRect(tileX, imgY + RoundedRaffMetrics::values.homeCoverHeight, tileWidth,
                             tileHeight - (imgY - tileY + RoundedRaffMetrics::values.homeCoverHeight), kRowRadius,
                             false, false, true, true, Color::LightGray);
  } else {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kRowRadius, Color::LightGray);
    renderer.drawCenteredText(titleFontId(), rect.y + rect.height / 2 - renderer.getLineHeight(titleFontId()) / 2,
                              tr(STR_NO_OPEN_BOOK));
  }
}

void RoundedRaffTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                      const std::function<std::string(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowHeight = renderer.getLineHeight(titleFontId()) + 20;  // 10px top + 10px bottom
  const int rowGap = kSelectableRowGap;
  const int rowStep = rowHeight + rowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelectedIndex = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelectedIndex / pageItems) * pageItems;
  const int menuTop = rect.y;
  const int textLineHeight = renderer.getLineHeight(titleFontId());
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const std::string label = buttonLabel(i);
    const int rowY = menuTop + (i - pageStartIndex) * rowStep;
    constexpr int kRowPaddingX = 40;  // 20px L/R
    const int maxLabelWidth = std::max(0, menuMaxWidth - kRowPaddingX);
    const std::string truncatedLabel =
        renderer.truncatedText(titleFontId(), label.c_str(), maxLabelWidth, EpdFontFamily::BOLD);
    const int rowWidth = std::min(
        menuMaxWidth,
        renderer.getTextWidth(titleFontId(), truncatedLabel.c_str(), EpdFontFamily::BOLD) + kRowPaddingX);
    const bool isSelected = selectedIndex == i;
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, kMenuRadius, isSelected ? Color::Black : Color::White);
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + kInteractiveInsetX;
    if (selectedIndex == i) {
      renderer.drawText(titleFontId(), textX, textY, truncatedLabel.c_str(), false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(titleFontId(), textX, textY, truncatedLabel.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, buttonCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                     int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(titleFontId());
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;

  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth - 1, lineY, thickness, true);
    return;
  }

  constexpr int hPadding = 8;
  const int lineW = textWidth + hPadding * 2;
  const int lineStart = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineStart, lineY, lineStart + lineW - 1, lineY, thickness, true);
}

void RoundedRaffTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                       const char* secondaryLabel, const KeyboardKeyType keyType,
                                       const bool inactiveSelection) const {
  constexpr int keyRadius = 10;
  const bool disabled = keyType == KeyboardKeyType::Disabled;
  const bool invert = isSelected && !inactiveSelection;

  if (isSelected) {
    const Color fillColor = (inactiveSelection || disabled) ? Color::LightGray : Color::Black;
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, keyRadius, fillColor);
  } else {
    if (disabled) {
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, keyRadius, Color::LightGray);
    } else {
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, keyRadius, Color::White);
    }
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, keyRadius, true);
  }

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = std::max(1, arrowLen / 2);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }

  if (label != nullptr && label[0] != '\0') {
    const int itemWidth = renderer.getTextWidth(titleFontId(), label);
    const int textX = rect.x + (rect.width - itemWidth) / 2;
    const int textY = rect.y + (rect.height - renderer.getLineHeight(titleFontId())) / 2;
    renderer.drawText(titleFontId(), textX, textY, label, !invert);
  }

  if (secondaryLabel != nullptr && secondaryLabel[0] != '\0') {
    const int secWidth = renderer.getTextWidth(subtitleFontId(), secondaryLabel);
    renderer.drawText(subtitleFontId(), rect.x + rect.width - secWidth - 3, rect.y + 1, secondaryLabel, !invert);
  }
}

void RoundedRaffTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                const std::function<std::string(int index)>& rowTitle,
                                const std::function<std::string(int index)>& rowSubtitle,
                                const std::function<UIIcon(int index)>& rowIcon,
                                const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                const std::function<bool(int index)>& rowDimmed, int titleFontId) const {
  (void)rowIcon;
  (void)highlightValue;
  (void)rowDimmed;
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);
  const int titleFont = titleFontId != 0 ? titleFontId : this->titleFontId();
  const auto titleStyle = titleFontId != 0 ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
  const int titleLineHeight = renderer.getLineHeight(titleFont);
  // A CJK subtitle must use the selected SD-card font as well. The built-in
  // SMALL_FONT_ID only contains the Latin UI glyphs.
  const int subtitleFont = renderer.isSdCardFont(titleFont) ? titleFont : subtitleFontId();
  const int subtitleLineHeight = renderer.getLineHeight(subtitleFont);
  constexpr int subtitleTopPadding = 10;
  constexpr int subtitleBottomPadding = 10;
  constexpr int subtitleInterLineGap = 4;
  const int subtitleRowHeight =
      subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
  const int rowHeight = hasSubtitle ? subtitleRowHeight : RoundedRaffMetrics::values.listRowHeight;
  const int rowStep = rowHeight + kSelectableRowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = std::max(0, selectedIndex / pageItems) * pageItems;

  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowWidth = rect.width - sidePadding * 2;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int rowY = rect.y + (i % pageItems) * rowStep;
    const bool isSelected = i == selectedIndex;
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, kRowRadius, isSelected ? Color::Black : Color::White);

    constexpr int kMinTitleWidth = 40;
    constexpr int kMinValueGap = kInteractiveInsetX;
    int textAreaWidth = rowWidth - kInteractiveInsetX * 2;
    if (rowValue) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValueWidth = std::max(0, rowWidth - kInteractiveInsetX * 2 - kMinValueGap - kMinTitleWidth);
        if (maxValueWidth > 0) {
          const std::string truncatedValue =
              renderer.truncatedText(this->titleFontId(), valueText.c_str(), maxValueWidth, EpdFontFamily::REGULAR);
          const int valueW =
              renderer.getTextWidth(this->titleFontId(), truncatedValue.c_str(), EpdFontFamily::REGULAR);
          renderer.drawText(this->titleFontId(), rowX + rowWidth - kInteractiveInsetX - valueW,
                            rowY + (rowHeight - renderer.getLineHeight(this->titleFontId())) / 2,
                            truncatedValue.c_str(),
                            !isSelected, EpdFontFamily::REGULAR);
          textAreaWidth = std::max(0, textAreaWidth - valueW - kMinValueGap);
        }
      }
    }

    if (hasSubtitle) {
      const std::string subtitleRaw = rowSubtitle(i);
      auto title = renderer.truncatedText(titleFont, rowTitle(i).c_str(), textAreaWidth, titleStyle);

      if (subtitleRaw.empty()) {
        // If there is no subtitle/author, center title vertically in the full row.
        const int centeredTitleY = rowY + (rowHeight - titleLineHeight) / 2;
        renderer.drawText(titleFont, rowX + kInteractiveInsetX, centeredTitleY, title.c_str(), !isSelected,
                          titleStyle);
      } else {
        const int titleY = rowY + subtitleTopPadding;
        const int subtitleY = titleY + titleLineHeight + subtitleInterLineGap;
        // Keep Latin subtitles in the built-in small font; only CJK text uses
        // the selected SD-card font.
        const int rowSubtitleFont = DynamicFont::fontForCjkText(renderer, subtitleRaw.c_str(), subtitleFontId());
        DynamicFont::prewarmIfSdFont(renderer, rowSubtitleFont, subtitleRaw);
        auto subtitle =
            renderer.truncatedText(rowSubtitleFont, subtitleRaw.c_str(), textAreaWidth, EpdFontFamily::REGULAR);
        renderer.drawText(titleFont, rowX + kInteractiveInsetX, titleY, title.c_str(), !isSelected, titleStyle);
        renderer.drawText(rowSubtitleFont, rowX + kInteractiveInsetX, subtitleY, subtitle.c_str(), !isSelected,
                          EpdFontFamily::REGULAR);
      }
    } else {
      auto title = renderer.truncatedText(titleFont, rowTitle(i).c_str(), textAreaWidth, titleStyle);
      renderer.drawText(titleFont, rowX + kInteractiveInsetX, rowY + (rowHeight - titleLineHeight) / 2, title.c_str(),
                        !isSelected, titleStyle);
    }
  }

  drawScrollBar(renderer, rect, itemCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                       const char* btn4) const {
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = 20;
  const int groupGap = 10;
  const int bottomMargin = 10;
  const int hintHeight = RoundedRaffMetrics::values.buttonHintsHeight - 10;  // 30px total guide height
  const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
  const int hintY = pageHeight - hintHeight - bottomMargin;
  const int textY = hintY + (hintHeight - renderer.getLineHeight(guideFontId())) / 2;

  const bool backDisabled = (btn1 == nullptr || btn1[0] == '\0');
  const int leftGroupX = sidePadding;
  const int rightGroupX = leftGroupX + groupWidth + groupGap;
  const std::string backLabel = backDisabled ? "" : std::string(btn1);
  // Callers should provide the button labels. If a label is not specified, it should render empty.
  const std::string selectText = (btn2 && btn2[0] != '\0') ? std::string(btn2) : "";
  const std::string upText = (btn3 && btn3[0] != '\0') ? std::string(btn3) : "";
  const std::string downText = (btn4 && btn4[0] != '\0') ? std::string(btn4) : "";

  // Button hints are rendered outside the FUI list renderer, so resolve CJK
  // labels explicitly.  This keeps labels such as the Chinese "词库"
  // consistent with headers and list rows when an SD-card UI font is active,
  // while still using the bundled UI face when no SD font is selected.
  const int fallbackFontId = guideFontId();
  const int backFontId = DynamicFont::fontForCjkText(renderer, backLabel.c_str(), fallbackFontId);
  const int selectFontId = DynamicFont::fontForCjkText(renderer, selectText.c_str(), fallbackFontId);
  const int upFontId = DynamicFont::fontForCjkText(renderer, upText.c_str(), fallbackFontId);
  const int downFontId = DynamicFont::fontForCjkText(renderer, downText.c_str(), fallbackFontId);
  DynamicFont::prewarmIfSdFont(renderer, backFontId, backLabel);
  DynamicFont::prewarmIfSdFont(renderer, selectFontId, selectText);
  DynamicFont::prewarmIfSdFont(renderer, upFontId, upText);
  DynamicFont::prewarmIfSdFont(renderer, downFontId, downText);

  // Ensure button hints always "win" visually even if other elements accidentally render into this area.
  renderer.fillRect(leftGroupX, hintY, groupWidth, hintHeight, false);
  renderer.fillRect(rightGroupX, hintY, groupWidth, hintHeight, false);

  renderer.drawRoundedRect(leftGroupX, hintY, groupWidth, hintHeight, 2, kBottomRadius, true);
  const int selectWidth = renderer.getTextWidth(selectFontId, selectText.c_str(), EpdFontFamily::REGULAR);
  const int downWidth = renderer.getTextWidth(downFontId, downText.c_str(), EpdFontFamily::REGULAR);
  constexpr int innerEdgePadding = 16;

  const int backX = leftGroupX + innerEdgePadding;
  const int selectX = leftGroupX + groupWidth - innerEdgePadding - selectWidth;
  const int upX = rightGroupX + innerEdgePadding;
  const int downX = rightGroupX + groupWidth - innerEdgePadding - downWidth;

  if (!backDisabled) {
    renderer.drawText(backFontId, backX, textY, backLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawText(selectFontId, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawRoundedRect(rightGroupX, hintY, groupWidth, hintHeight, 2, kBottomRadius, true);

  renderer.drawText(upFontId, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(downFontId, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.setOrientation(origOrientation);
}
