#pragma once

#include "activities/Activity.h"
#include "games/shushan/ShushanGame.h"
#include "games/shushan/ShushanGameStore.h"
#include "util/ButtonNavigator.h"

class ShushanGameActivity final : public Activity {
  shushan::Game game;
  shushan::GameStore store;
  shushan::GameState savedState;
  ButtonNavigator buttonNavigator;
  shushan::ActionResult lastAction = shushan::ActionResult::INVALID;
  int selectedIndex = 0;
  bool hasValidSave = false;
  bool saveFailed = false;

  void startNewGame();
  void continueGame();
  void confirmNewGame();
  void saveGame();
  void navigate(int itemCount);
  void renderTitle();
  void renderStory();
  void renderBattle();
  void renderEnding();
  void drawStatus(int y);

 public:
  explicit ShushanGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ShushanGame", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool allowPowerSaving() override { return true; }
  void render(RenderLock&&) override;
};
