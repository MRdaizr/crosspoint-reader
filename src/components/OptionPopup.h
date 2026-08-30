#pragma once

#include <I18n.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Modal option picker used by the WeRead screens. It accepts both touch and
// the existing NavPrevious/NavNext/Confirm/Back mapping.
class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, const int optionCount, const int currentIndex,
            std::function<void(int)> onSelect) {
    title_ = I18N.get(titleId);
    options_.clear();
    for (int i = 0; i < optionCount; ++i) options_.emplace_back(I18N.get(optionIds[i]));
    finishShow(currentIndex, std::move(onSelect));
  }

  void show(const char* title, const StrId* optionIds, const int optionCount, const int currentIndex,
            std::function<void(int)> onSelect) {
    title_ = title ? title : "";
    options_.clear();
    for (int i = 0; i < optionCount; ++i) options_.emplace_back(I18N.get(optionIds[i]));
    finishShow(currentIndex, std::move(onSelect));
  }

  // Runtime labels are used by point-size and margin pickers.  Keeping the
  // same modal/input path avoids each settings page reimplementing a picker.
  void show(StrId titleId, const std::vector<std::string>& options, const int currentIndex,
            std::function<void(int)> onSelect) {
    title_ = I18N.get(titleId);
    options_ = options;
    finishShow(currentIndex, std::move(onSelect));
  }

  bool handleInput(GfxRenderer& renderer, MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active_) return false;

    const int count = static_cast<int>(options_.size());
    if (count <= 0) {
      active_ = false;
      return true;
    }
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + 16;
    const int dialogWidth = std::min(renderer.getScreenWidth() - 30, 400);
    const int dialogHeight = renderer.getLineHeight(UI_12_FONT_ID) + 20 + rowHeight * count;
    const int dialogX = std::max(0, (renderer.getScreenWidth() - dialogWidth) / 2);
    const int dialogY = std::max(0, (renderer.getScreenHeight() - dialogHeight) / 2);
    int touchX = 0;
    int touchY = 0;
    const bool touchDown = input.wasScreenTouchDown(touchX, touchY);
    const bool touchTap = !touchDown && input.wasScreenTapped(touchX, touchY);
    if (touchDown || touchTap) {
      const bool inside = touchX >= dialogX && touchX < dialogX + dialogWidth && touchY >= dialogY &&
                          touchY < dialogY + dialogHeight;
      if (inside) {
        const int firstRowY = dialogY + renderer.getLineHeight(UI_12_FONT_ID) + 20;
        if (touchY >= firstRowY) {
          const int index = (touchY - firstRowY) / rowHeight;
          if (index >= 0 && index < count) {
            selected_ = index;
            if (touchTap) {
              active_ = false;
              if (onSelect_) onSelect_(selected_);
            }
            requestUpdate();
            return true;
          }
        }
        return true;
      }
      if (touchTap) {
        active_ = false;
        requestUpdate();
        return true;
      }
    }
    if (input.wasReleased(MappedInputManager::Button::NavPrevious)) {
      selected_ = (selected_ + count - 1) % count;
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::NavNext)) {
      selected_ = (selected_ + 1) % count;
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      active_ = false;
      requestUpdate();
      return true;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      if (ignoreInitialConfirmRelease_) {
        ignoreInitialConfirmRelease_ = false;
        return true;
      }
      active_ = false;
      if (onSelect_) onSelect_(selected_);
      requestUpdate();
      return true;
    }
    if (ignoreInitialConfirmRelease_ && !input.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreInitialConfirmRelease_ = false;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    return processRenderImpl(renderer, input, true, true);
  }

  // Render the popup over an already painted page. ReaderToolbarUi uses this
  // path so a point-size/line-spacing picker does not clear the EPUB page
  // beneath the bottom sheet. The caller owns the final refresh because it may
  // need to composite other overlay controls in the same frame.
  bool processRenderOverlay(GfxRenderer& renderer, const MappedInputManager& input) const {
    return processRenderImpl(renderer, input, false, false);
  }

 private:
  bool processRenderImpl(GfxRenderer& renderer, const MappedInputManager& input, const bool clearBeforeDraw,
                         const bool refreshDisplay) const {
    if (!active_) return false;

    if (clearBeforeDraw) renderer.clearScreen();
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + 16;
    const int dialogWidth = std::min(screenWidth - 30, 400);
    const int dialogHeight = renderer.getLineHeight(UI_12_FONT_ID) + 20 + rowHeight * static_cast<int>(options_.size());
    const int x = std::max(0, (screenWidth - dialogWidth) / 2);
    const int y = std::max(0, (screenHeight - dialogHeight) / 2);

    renderer.fillRect(x, y, dialogWidth, dialogHeight, false);
    renderer.drawRect(x, y, dialogWidth, dialogHeight);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 8, title_.c_str(), true, EpdFontFamily::BOLD);
    for (size_t i = 0; i < options_.size(); ++i) {
      const int rowY = y + renderer.getLineHeight(UI_12_FONT_ID) + 20 + static_cast<int>(i) * rowHeight;
      const bool selected = static_cast<int>(i) == selected_;
      if (selected) renderer.fillRect(x + 2, rowY, dialogWidth - 4, rowHeight);
      renderer.drawCenteredText(UI_10_FONT_ID, rowY + 8, options_[i].c_str(), !selected);
    }

    const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (refreshDisplay) renderer.displayBuffer();
    return true;
  }

 public:
  bool isActive() const { return active_; }
  void dismiss() {
    active_ = false;
    ignoreInitialConfirmRelease_ = false;
    onSelect_ = nullptr;
  }

 private:
  void finishShow(const int currentIndex, std::function<void(int)> onSelect) {
    selected_ = std::clamp(currentIndex, 0, std::max(0, static_cast<int>(options_.size()) - 1));
    onSelect_ = std::move(onSelect);
    ignoreInitialConfirmRelease_ = true;
    active_ = !options_.empty();
  }

  bool active_ = false;
  bool ignoreInitialConfirmRelease_ = false;
  int selected_ = 0;
  std::string title_;
  std::vector<std::string> options_;
  std::function<void(int)> onSelect_;
};
