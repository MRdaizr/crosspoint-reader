#pragma once

#include <Txt.h>

#include <atomic>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"
#include "activities/reader/TxtReaderMenuActivity.h"

class TxtReaderActivity final : public ReaderActivity {
  struct PageLayout {
    int page = -1;
    size_t nextOffset = 0;
    std::vector<std::string> lines;
  };

  enum class NextPagePrepareStage : uint8_t { NONE, LAYOUT, FONT };

  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;
  bool pendingScreenshot = false;
  bool pageIndexComplete = false;
  // Written by the input task and consumed by the render task. Coalescing to
  // one request prevents a long background layout from advancing page state
  // more than once before the display catches up.
  std::atomic<int8_t> pendingPageTurn{0};
  std::atomic<int16_t> pendingPercentJump{-1};
  bool indexBuildOnlyRequested = false;
  bool indexProgressRefreshPending = false;
  bool nextPagePrepareRequested = false;
  NextPagePrepareStage nextPagePrepareStage = NextPagePrepareStage::NONE;
  int nextPagePrepareTarget = -1;
  size_t nextPagePrepareNextOffset = 0;
  std::vector<std::string> nextPagePrepareLines;
  int preparedPage = -1;
  size_t preparedPageNextOffset = 0;
  std::vector<std::string> preparedPageLines;
  std::vector<PageLayout> pageLayoutCache;
  // A local page window used after an approximate percent jump. The canonical
  // pageOffsets vector remains a strictly sequential index from file start.
  bool approximatePosition = false;
  std::vector<size_t> approximatePageOffsets;
  int approximateLocalPage = 0;
  int approximateBasePage = 0;
  int approximateTotalPages = 1;
  size_t pendingApproximateResumeOffset = 0;
  int pendingResumePageTarget = -1;
  int pendingPercentTarget = -1;
  uint8_t indexProgressPercent = 100;
  uint8_t lastDisplayedIndexProgressPercent = 255;
  size_t lastCheckpointPageCount = 0;
  unsigned long lastIndexBuildTick = 0UL;
  unsigned long lastIndexProgressRefreshMs = 0UL;
  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(bool fontPrewarmed = false);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void jumpToPercent(int percent);
  void applyPercentJump(int percent);
  void beginApproximatePosition(int percent, size_t offset = 0);
  size_t alignApproximateOffset(size_t offset) const;
  bool ensureApproximatePage(int page);
  void reconcileApproximatePosition();
  int displayPageKey() const;
  int nextDisplayPageKey() const;
  int displayedPage() const;
  int displayedTotalPages() const;
  size_t displayedOffset() const;
  void onReaderMenuConfirm(TxtReaderMenuActivity::MenuAction action);

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool ensurePageIndexed(int page);
  bool indexNextPage();
  void updateTotalPages();
  bool loadPageIndexCache();
  bool loadPartialPageIndexCache();
  void savePageIndexCache() const;
  bool savePartialPageIndexCache();
  void removePartialPageIndexCache() const;
  void buildPageIndexSlice();
  void prepareNextPage();
  void cancelNextPagePreparation();
  bool applyPendingPageTurn();
  void scheduleBackgroundWork();
  const PageLayout* findPageLayout(int page) const;
  void cachePageLayout(int page, size_t nextOffset, const std::vector<std::string>& lines);
  void clearPageLayouts();
  void updateIndexProgress(bool requestRefresh);
  void saveProgress();
  void loadProgress();
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : ReaderActivity("TxtReader", renderer, mappedInput, txt ? txt->getPath() : ""), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  ScreenshotInfo getScreenshotInfo() const override;
};
