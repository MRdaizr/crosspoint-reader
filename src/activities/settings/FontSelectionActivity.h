#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/themes/BaseTheme.h"

class FontSelectionActivity final : public UiListActivity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  int listCount() const override { return static_cast<int>(fonts_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override;
  int getFontIdForPreview(int index) const;
  void renderPreviewPane(int top, int height, int fontId, const char* fontName) const;

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  const SdCardFontRegistry* registry_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
  int previewFontIndex_ = 0;
  uint8_t originalFontFamily_ = 0;
  char originalSdFontFamilyName_[32] = {};

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
