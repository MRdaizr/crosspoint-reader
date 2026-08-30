#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TextSettingsPreview.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"

// Unified reader text settings.  Changes are applied immediately and the
// preview is laid out with the same ParsedText path used by EPUB pages.
class TextSettingsActivity final : public UiTabListActivity {
 public:
  enum class Tab : uint8_t { Family, Size, Layout, Style, Count };

  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       Tab initialTab = Tab::Family);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  enum class LayoutRow { LineSpacing, ParaSpacing, Alignment, ScreenMargin, Count };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  int listCount() const override;
  int tabCount() const override { return static_cast<int>(Tab::Count); }
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  bool handleCustomInput() override;

  void applyFamily(int index);
  void applySize(int index);
  void rebuildSizeList();
  void rebuildRowItems();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  void activateRow(int row);
  std::string layoutValueText(int row) const;
  std::string styleValueText(int row) const;
  const char* confirmLabelText() const;
  bool focusedRowHasNoPreview() const;
  void switchTab(int direction = 1);

  struct FontEntry { std::string name; bool isBuiltin; uint8_t settingIndex; };
  struct SizeEntry { std::string name; uint8_t pointSize; };

  const SdCardFontRegistry* registry_;
  OptionPopup optionPopup_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  textsettings::PreviewLayout previewLayout_;
  Tab tab_;
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;
  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
