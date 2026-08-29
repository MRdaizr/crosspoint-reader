#pragma once
#include <Epub.h>
#include <I18n.h>

#include <memory>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex = 0;
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;

  // Total TOC items count
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex)
      : UiListActivity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex) {}
  void onEnter() override;
 private:
  int listCount() const override { return getTotalItems(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_SELECT_CHAPTER); }
};
