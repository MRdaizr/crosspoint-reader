#pragma once

#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryQuery.h>

#include <memory>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"

// One visible page and an ordinal page, plus the small recent-book overlay.
// No complete shelf of titles, authors, or paths is retained in RAM.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiTabListActivity("Library", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override { return rebuildPending || rebuilding; }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int row) override;
  int tabCount() const override { return 3; }
  int activeTab() const override { return tab; }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  static constexpr int TOOLS = 2;
  static constexpr size_t MAX_PINNED = 10;  // current RecentBooksStore capacity
  struct DisplayRow {
    char title[256] = {};
    char author[129] = {};
  };
  static void provideRow(void*, uint16_t, freeink::ui::ListItem&);
  static const char* prewarmText(const void*, uint32_t);
  static bool cancelBuild(void*);
  static void buildProgress(void*, library::BuildPhase, uint16_t, uint16_t);
  library::SortOrder order() const;
  bool hasPins() const;
  int pinnedCount() const;
  void resolvePinned();
  uint16_t ordinalForBook(int row);
  void refreshQuery();
  void rebuild();
  void openSearch();
  void promptRebuild();
  bool loadWindow(int first, int count);
  void invalidateWindow();

  library::LibraryIndexFile index;
  std::unique_ptr<library::LibraryQuery> queryEngine;
  std::unique_ptr<DisplayRow[]> rows;
  library::QueryPage page;
  char query[library::LIBRARY_QUERY_BYTES + 1] = {};
  char header[192] = {};
  char path[513] = {};  // exact raw folder + separator + raw basename
  uint16_t matched = 0;
  uint16_t pinnedAsc[MAX_PINNED] = {};
  uint16_t overlap[MAX_PINNED] = {};
  freeink::ui::ListProps listProps;  // SDK styles exceed the task stack budget
  uint8_t pins = 0;
  uint8_t overlaps = 0;
  uint8_t descending = 1;  // newest arrivals first
  int tab = 0;
  int windowFirst = -1;
  int windowCount = 0;
  library::SortOrder windowOrder = library::SortOrder::RecentDesc;
  bool rebuildPending = false;
  bool rebuilding = false;
  bool cancelArmed = false;
  bool swallowConfirm = false;
  bool swallowBack = false;
  bool dataFailed = false;
  StrId status = StrId::STR_LIBRARY;
  uint32_t lastProgressMs = 0;
  library::BuildPhase lastPhase = library::BuildPhase::Complete;
};
