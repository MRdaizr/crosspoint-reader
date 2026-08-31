#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "activities/reader/EndOfBookOptions.h"
#include <Epub/ReaderRenderSpec.h>

class Epub;
class Xtc;
class Txt;

// ReaderActivity is the common lifecycle/metadata layer for the EPUB, TXT and
// XTC readers.  The concrete readers still own their format-specific render
// loops (EPUB has overlays and incremental sections, TXT has a streaming
// indexer, and XTC has a pre-rendered page store), but all three now enter and
// leave through the same hooks and expose the same page-turn contract.
class ReaderActivity : public Activity {
 protected:
  ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                 bool allowFastInitialRefresh = false)
      : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)),
        allowFastInitialRefresh_(allowFastInitialRefresh) {}

  std::string bookPath;
  // Kept in the common layer so all format readers can opt into the same
  // first-open refresh cadence when launched by ActivityManager. Existing X4
  // callers default to false and retain their current refresh behavior.
  bool allowFastInitialRefresh_ = false;
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  // Format hooks.  Keeping these in the base class makes lifecycle and page
  // navigation code independent of the concrete reader's storage format.
  virtual bool loadBook() = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return {}; }
  virtual std::string getBookThumbBmpPath() const { return {}; }
  virtual uint8_t getInitialProgressPercent() const { return 0; }
  virtual void onBookEntered() {}
  virtual void onBookExited() {}
  virtual bool pageTurn(bool isForward) = 0;
  virtual bool skipPages(int amount) { return pageTurn(amount > 0); }
  virtual void applyInitialOrientation();

  void clearEndOfBookOptionsIfNeeded(bool atEndOfBook);
  bool handleEndOfBookMenu(bool atEndOfBook, bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool atEndOfBook, bool prevTriggered, bool nextTriggered);
  bool handleBackNavigation(const char* filePath);
  void renderEndOfBook(const MappedInputManager& input);
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}

  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);

  // Shared render contract for EPUB/TXT/XTC readers.  Concrete readers can
  // pass this object to Section overloads (or their format-specific layout
  // engines) without each screen re-reading settings independently.
  ReaderRenderSpec currentReaderRenderSpec() const;
  ReaderRenderSpec currentReaderRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<ReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                std::string path, bool allowFastInitialRefresh = false);
  void onEnter() override;
  void onExit() override;
  bool isReaderActivity() const override { return true; }
};
