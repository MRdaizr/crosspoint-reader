#pragma once

#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

class RoundedRaffExtTheme : public RoundedRaffTheme {
 protected:
  int subtitleFontId() const override { return UI_12_FONT_ID; }
  int guideFontId() const override { return UI_12_FONT_ID; }
};
