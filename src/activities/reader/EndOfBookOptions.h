#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// Shared end-of-book screen for EPUB, TXT and XTC readers.  It keeps the
// historical plain "End of book" screen when no sibling follows, and adds a
// bounded list of the next books in the same folder when available.
class EndOfBookOptions final : private UiAppHost {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };
  static constexpr size_t MAX_SUGGESTIONS = 3;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Must be called from the render task.  The release store publishes both
  // the names and the FUI row storage to the input task atomically.
  void loadOnce(const std::string& currentBookPath);
  bool menuActive() const;
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);
  void render(GfxRenderer& renderer, const MappedInputManager& input);

 private:
  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);
  void buildRowItems();
  std::string fullPath(size_t index) const;

  GfxRenderer& renderer;
  std::string folder;
  std::vector<std::string> names;
  int selector = 0;
  std::atomic<bool> isLoaded{false};

  static constexpr size_t MAX_ROWS = MAX_SUGGESTIONS + 1;
  std::string rowLabels[MAX_ROWS];
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  size_t rowCount = 0;
  int tappedRow = -1;
};
