#pragma once
#include <Epub.h>
#include <I18n.h>

#include <memory>

#include "../../BookmarkEntry.h"
#include "../UiListActivity.h"

class EpubReaderBookmarksActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  std::vector<BookmarkEntry> bookmarks;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete
  int deleteIndex = 0;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowSubtitles;
  std::vector<freeink::ui::ListItem> rowItems;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : UiListActivity("EpubReaderBookmarks", renderer, mappedInput, true), epub(epub), epubPath(epubPath) {}
  void onEnter() override;

 private:
  int listCount() const override { return confirmingDelete ? (bookmarks.empty() ? 0 : 1) : static_cast<int>(bookmarks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_BOOKMARKS); }
  void drawFooter() override;
};
