#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const BodyPlacement bodyPlacement)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body), bodyPlacement(bodyPlacement) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  // Keep explanatory text in the upper part of the screen so the touch/keys
  // confirmation popup remains readable and consistently placed.
  startY = renderer.getScreenHeight() / 6;

  const StrId optionIds[] = {StrId::STR_CANCEL, StrId::STR_CONFIRM};
  const char* popupTitle = bodyPlacement == BodyPlacement::Page ? safeHeading.c_str() : safeBody.c_str();
  confirmPopup.show(popupTitle, optionIds, 2, 0, [this](const int index) {
    ActivityResult result;
    result.isCancelled = index != 1;
    setResult(std::move(result));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (bodyPlacement == BodyPlacement::Page && !safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(renderer, mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back or outside tap): cancel.
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
