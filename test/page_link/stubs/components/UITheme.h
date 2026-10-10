#pragma once
struct LinkTestTheme {
  template <typename Renderer>
  void drawButtonHints(Renderer&, const char*, const char*, const char*, const char*) const {}
};
inline LinkTestTheme linkTestTheme;
#define GUI linkTestTheme
