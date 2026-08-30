#pragma once

#include <Epub/FootnoteEntry.h>

#include <I18n.h>

#include <cstring>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderFootnotesActivity final : public UiListActivity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes)
      : UiListActivity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes) {}

  void onEnter() override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;

  int listCount() const override { return static_cast<int>(footnotes.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_FOOTNOTES); }
  void drawFooter() override;
};
