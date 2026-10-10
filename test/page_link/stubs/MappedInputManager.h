#pragma once
class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Power, ScreenLeft, ScreenRight, ScreenUp, ScreenDown };
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };
  int pressed = -1, released = -1, tapX = 0, tapY = 0;
  bool tap = false;
  bool wasPressed(Button b) const { return pressed == static_cast<int>(b); }
  bool wasReleased(Button b) const { return released == static_cast<int>(b); }
  bool wasScreenTapped(int& x, int& y) const {
    x = tapX;
    y = tapY;
    return tap;
  }
  Labels mapDirectionalLabels(const char* a, const char* b, const char* c, const char* d, const char*,
                              const char*) const {
    return {a, b, c, d};
  }
};
