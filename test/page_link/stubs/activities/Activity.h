#pragma once
#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <activities/ActivityResult.h>
struct RenderLock {};
class Activity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  ActivityResult result;
  int updates = 0;
  bool finished = false;
  Activity(const char*, GfxRenderer& r, MappedInputManager& input) : renderer(r), mappedInput(input) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  virtual bool handleForcedRefresh() { return false; }
  void requestUpdate() { ++updates; }
  void setResult(ActivityResult&& value) { result = std::move(value); }
  void finish() { finished = true; }
};
