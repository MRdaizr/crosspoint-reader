#pragma once
#include <Xtc.h>
#include <I18n.h>

#include <memory>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class XtcReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;

  int findChapterIndexForPage(uint32_t page) const;

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage)
      : UiListActivity("XtcReaderChapterSelection", renderer, mappedInput), xtc(xtc), currentPage(currentPage) {}
  void onEnter() override;
 private:
  int listCount() const override { return xtc ? static_cast<int>(xtc->getChapters().size()) : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_SELECT_CHAPTER); }
};
