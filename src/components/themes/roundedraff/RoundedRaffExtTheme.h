#pragma once

#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

class RoundedRaffExtTheme : public RoundedRaffTheme {
 protected:
  int subtitleFontId() const override { return UI_12_FONT_ID; }
  int guideFontId() const override { return UI_12_FONT_ID; }

  // RoundedRaffExt uses a text-first menu layout: no leading icons on any
  // home or extension row. Keep the semantic slot in the base API so other
  // themes can opt into per-row icon policies later.
  bool showsFuiMenuIcon(FuiMenuIconSlot) const override { return false; }
};
