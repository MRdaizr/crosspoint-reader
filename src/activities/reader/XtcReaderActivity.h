/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <string>
#include <utility>

#include "ReaderActivity.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();
  bool loadBook() override;
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : std::string{}; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : std::string{}; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : std::string{}; }
  void renderBook() override;
  void onEndOfBookRendered() override;
  uint8_t getInitialProgressPercent() const override {
    return xtc && xtc->getPageCount() ? xtc->calculateProgress(currentPage) : 0;
  }
  void onBookEntered() override;
  void onBookExited() override;
  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh = false)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             bool allowFastInitialRefresh = false)
      : ReaderActivity("XtcReader", renderer, mappedInput, xtc ? xtc->getPath() : "", allowFastInitialRefresh),
        xtc(std::move(xtc)) {}
  void loop() override;
  ScreenshotInfo getScreenshotInfo() const override;
};
