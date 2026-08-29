#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/reader/EndOfBookOptions.h"

class Epub;
class Xtc;
class Txt;

// ReaderActivity remains the format dispatcher used by the existing activity
// manager, and also provides the shared reader end-of-book layer to the EPUB,
// TXT and XTC activities.  Keeping the dispatcher entry point avoids changing
// the X4 activity stack while the format readers adopt the common behavior.
class ReaderActivity : public Activity {
 protected:
  // Constructor for concrete format readers.  The public constructor below is
  // retained as the dispatcher entry point used by ActivityManager.
  ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
      : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)) {}

  std::string bookPath;
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  void clearEndOfBookOptionsIfNeeded(bool atEndOfBook);
  bool handleEndOfBookMenu(bool atEndOfBook, bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool atEndOfBook, bool prevTriggered, bool nextTriggered);
  void renderEndOfBook(const MappedInputManager& input);
  virtual bool isAtEndOfBook() const { return false; }
  virtual void onReturnFromEndOfBook() {}

  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
