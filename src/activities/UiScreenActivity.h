#pragma once

#include "Activity.h"
#include "RenderLock.h"
#include "components/UiAppHost.h"

// Common FUI host for screens that are not naturally a single scrolling list.
// It owns the same lifecycle and interaction-table handshake as UiListActivity
// while leaving the screen body and legacy model/input logic to the activity.
class UiScreenActivity : public Activity, protected UiAppHost {
 public:
  UiScreenActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                   bool wantsTouchLongPress = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 protected:
  using UiScreen = UiAppHost::UiScreen;

  virtual void buildScreen(UiScreen& screen) = 0;
  virtual bool handleFuiButtons();
  virtual bool handleFuiCustomInput();
  virtual void drawFuiChrome();
  virtual void drawFuiFooter();

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  bool wantsTouchLongPress = false;
};

