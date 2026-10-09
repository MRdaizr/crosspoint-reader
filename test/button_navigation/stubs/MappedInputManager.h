#pragma once
#include <cstdint>
inline uint32_t navigatorTestMillis = 0;
inline uint32_t millis() { return navigatorTestMillis; }
class MappedInputManager {
 public:
  enum class Button { NavNext, NavPrevious, Left, Right };
  bool down[4] = {}, pressed[4] = {}, released[4] = {};
  uint32_t heldMs = 0;
  bool isPressed(Button button) const { return down[static_cast<unsigned>(button)]; }
  bool wasPressed(Button button) const { return pressed[static_cast<unsigned>(button)]; }
  bool wasReleased(Button button) const { return released[static_cast<unsigned>(button)]; }
  uint32_t getHeldTime() const { return heldMs; }
};
