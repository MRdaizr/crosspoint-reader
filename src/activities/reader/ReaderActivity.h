#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "activities/reader/EndOfBookOptions.h"
#include "CrossPointSettings.h"
#include <Epub/ReaderRenderSpec.h>

class Epub;
class Xtc;
class Txt;

// ReaderActivity is the common lifecycle/metadata and render boundary for the
// EPUB, TXT and XTC readers. Each format still owns its input loop and layout
// engine, but activity entry/exit, refresh state and end-page rendering are
// shared so those paths cannot drift between formats.
class ReaderActivity : public Activity {
 protected:
  ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                 bool allowFastInitialRefresh = false)
      : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)),
        allowFastInitialRefresh_(allowFastInitialRefresh) {
    if (allowFastInitialRefresh_) {
      const int refreshFrequency = SETTINGS.getRefreshFrequency();
      pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
    }
  }

  std::string bookPath;
  // Refresh cadence belongs to the reader session, not to a concrete file
  // format. Keeping it here prevents TXT/EPUB/XTC from drifting when a manual
  // refresh is requested while an activity is on the stack.
  int pagesUntilFullRefresh = 0;
  bool forcedRefreshPending = false;
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
  virtual void renderBook() = 0;

  void clearEndOfBookOptionsIfNeeded(bool atEndOfBook);
  bool handleEndOfBookMenu(bool atEndOfBook, bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool atEndOfBook, bool prevTriggered, bool nextTriggered);
  bool handleBackNavigation(const char* filePath);
  void renderEndOfBook(const MappedInputManager& input);
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}
  // Called after the end-of-book UI has been composed but before it is sent
  // to the panel. Format readers use this for completion bookkeeping and
  // transient messages without duplicating the end-page render path.
  virtual void onEndOfBookRendered() {}

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
  bool handleForcedRefresh() final;
  void render(RenderLock&& lock) final;
  bool isReaderActivity() const override { return true; }
};
