#include "UiScreenActivity.h"

#include "MappedInputManager.h"

UiScreenActivity::UiScreenActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiScreenActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&UiScreenActivity::screenTrampoline, this);
  requestUpdate();
}

void UiScreenActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

void UiScreenActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiScreenActivity*>(user)->buildScreen(screen);
}

void UiScreenActivity::loop() {
  if (handleFuiCustomInput()) return;
  const auto routed = routeTouch(mappedInput, wantsTouchLongPress);
  if (routed.routed) return;
  handleFuiButtons();
}

void UiScreenActivity::render(RenderLock&& lock) {
  (void)lock;
  renderer.clearScreen();
  drawFuiChrome();
  renderUi();
  drawFuiFooter();
  renderer.displayBuffer();
}

bool UiScreenActivity::handleFuiButtons() { return false; }

bool UiScreenActivity::handleFuiCustomInput() { return false; }

void UiScreenActivity::drawFuiChrome() {}

void UiScreenActivity::drawFuiFooter() {}
