#pragma once
#include <Epub.h>

#include <atomic>

#include "activities/Activity.h"

// Preparation is NOT a ReaderActivity: cancellation/failure must not start a
// reading session or save a new page merely because a book was selected.
class TxtReflowPrepareActivity : public Activity {
 public:
  TxtReflowPrepareActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path,
                           bool allowFastInitialRefresh = false);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return state.load() == State::Working; }
  bool handleHomeGesture() override;

 private:
  enum class State { Pending, Working, Ready, Failed, Compatibility, Cancelled };
  std::string path;
  bool fastRefresh;
  std::unique_ptr<Epub> readyEpub;
  std::atomic<State> state{State::Pending};
  std::atomic<bool> cancelRequested{false};
  std::atomic<uint8_t> percent{0};
  uint32_t lastDraw = 0;
  uint8_t lastPercent = 255;
  bool homeRequested = false;  // loop-owned
  static bool cancelled(void* context);
  static void progress(void* context, uint32_t bytes, uint32_t total);
  void drawStatus();
};
