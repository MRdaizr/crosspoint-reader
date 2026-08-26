#include "WeReadActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <string>

#include "CrossPointState.h"
#include "CrossPointSettings.h"
#include "NetworkStartup.h"
#include "SilentRestart.h"
#include "WeReadBrowseActivity.h"
#include "activities/apps/weread/WeReadTouchGeometry.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "util/DynamicFont.h"
#include "util/TimeUtils.h"
#include "HalClock.h"

namespace {

static_assert(sizeof(WeReadClient::Operation) <= 8 * 1024, "WeRead workspace exceeds its fixed heap budget");

enum class ManageAction : uint8_t { Refresh, ClearCache, Logout };

struct ManageEntry {
  StrId title;
  ManageAction action;
};

constexpr ManageEntry kManageEntries[] = {
    {StrId::STR_WEREAD_MENU_REFRESH, ManageAction::Refresh},
    {StrId::STR_WEREAD_MENU_CLEAR_CACHE, ManageAction::ClearCache},
    {StrId::STR_WEREAD_MENU_LOGOUT, ManageAction::Logout},
};

constexpr StrId kDisclaimerParagraphs[] = {
    StrId::STR_WEREAD_DISCLAIMER_PARAGRAPH_1,
    StrId::STR_WEREAD_DISCLAIMER_PARAGRAPH_2,
    StrId::STR_WEREAD_DISCLAIMER_PARAGRAPH_3,
};

constexpr StrId kDisclaimerActions[] = {
    StrId::STR_WEREAD_DISCLAIMER_CANCEL,
    StrId::STR_WEREAD_DISCLAIMER_CONFIRM,
};

constexpr StrId kCacheScopeOptions[] = {
    StrId::STR_WEREAD_CACHE_WHOLE_BOOK,
    StrId::STR_WEREAD_CACHE_CHAPTER_RANGE,
};

constexpr StrId kShelfRefreshOptions[] = {
    StrId::STR_NO,
    StrId::STR_YES,
};

constexpr StrId kPostProcessWaitingLines[] = {
    StrId::STR_WEREAD_POST_PROCESS_WAIT_LINE_1,
    StrId::STR_WEREAD_POST_PROCESS_WAIT_LINE_2,
};

constexpr StrId kPostProcessLongWaitLines[] = {
    StrId::STR_WEREAD_POST_PROCESS_LONG_WAIT_LINE_1,
    StrId::STR_WEREAD_POST_PROCESS_LONG_WAIT_LINE_2,
    StrId::STR_WEREAD_POST_PROCESS_LONG_WAIT_LINE_3,
    StrId::STR_WEREAD_POST_PROCESS_LONG_WAIT_LINE_4,
};

constexpr int kDisclaimerActionCount = static_cast<int>(sizeof(kDisclaimerActions) / sizeof(kDisclaimerActions[0]));
constexpr int kDisclaimerParagraphCount =
    static_cast<int>(sizeof(kDisclaimerParagraphs) / sizeof(kDisclaimerParagraphs[0]));
constexpr int kMinimumDisclaimerActionGap = 4;
constexpr int kManageEntryCount = static_cast<int>(sizeof(kManageEntries) / sizeof(kManageEntries[0]));
constexpr size_t kMainTabCount = 2;
constexpr int kDetailCoverWidth = 96;
constexpr int kDetailCoverHeight = 140;
constexpr int kPortraitShelfColumns = 1;
constexpr int kPortraitShelfRows = 1;
constexpr int kLandscapeShelfColumns = 1;
constexpr int kLandscapeShelfRows = 1;
constexpr unsigned long kShelfPageHoldMs = 700;
constexpr int kNoShelfSelection = -1;
constexpr size_t kIntroAdvanceChunkBytes = 4096;
constexpr size_t kIntroLineBufferBytes = 192;

constexpr int disclaimerActionGap(const int width, const int themeSpacing) {
  return std::min(std::max(kMinimumDisclaimerActionGap, themeSpacing), std::max(0, width - kDisclaimerActionCount));
}

static_assert(disclaimerActionGap(200, 0) == kMinimumDisclaimerActionGap);
static_assert(disclaimerActionGap(200, 16) == 16);
static_assert(disclaimerActionGap(kDisclaimerActionCount, 0) == 0);

constexpr int previousShelfIndexOrTab(const int currentIndex) {
  return currentIndex > 0 ? currentIndex - 1 : kNoShelfSelection;
}

static_assert(previousShelfIndexOrTab(0) == kNoShelfSelection);
static_assert(previousShelfIndexOrTab(4) == 3);

constexpr bool canIncrementShelfFrame(const int frameSelection, const int frameItemsPerPage, const int selectedIndex,
                                      const int itemsPerPage) {
  return frameItemsPerPage == itemsPerPage && frameSelection >= 0 && selectedIndex >= 0 &&
         frameSelection != selectedIndex && itemsPerPage > 0 &&
         frameSelection / itemsPerPage == selectedIndex / itemsPerPage;
}

static_assert(!canIncrementShelfFrame(kNoShelfSelection, 9, 0, 9));
static_assert(!canIncrementShelfFrame(3, 9, 3, 9));
static_assert(canIncrementShelfFrame(3, 9, 4, 9));
static_assert(!canIncrementShelfFrame(8, 9, 9, 9));
static_assert(!canIncrementShelfFrame(3, 9, 4, 10));

int dynamicRemoteFontId(GfxRenderer& renderer, const char* text, const int fallbackFontId) {
  const int fontId = DynamicFont::fontForCjkText(renderer, text, fallbackFontId);
  if (text && text[0]) DynamicFont::prewarmIfSdFont(renderer, fontId, std::string(text));
  return fontId;
}

int dynamicRemoteBodyFontId(GfxRenderer& renderer, const int fallbackFontId, const char* context) {
  sdFontSystem.ensureLoaded(renderer);
  const int sdFontId = sdFontSystem.currentFontId();
  const bool registered = renderer.isSdCardFont(sdFontId);
  const bool inFontMap = renderer.getFontMap().count(sdFontId) != 0;
  const int resolvedFontId = registered ? sdFontId : fallbackFontId;
  LOG_DBG("WR", "intro font[%s]: configured='%s' sdId=%d registered=%u map=%u resolved=%d fallback=%d",
          context ? context : "?", SETTINGS.sdFontFamilyName, sdFontId, registered ? 1U : 0U, inFontMap ? 1U : 0U,
          resolvedFontId, fallbackFontId);
  return resolvedFontId;
}

EpdFontFamily::Style dynamicRemoteFontStyle(const GfxRenderer& renderer, const int fontId,
                                            const EpdFontFamily::Style builtInStyle = EpdFontFamily::BOLD) {
  return renderer.isSdCardFont(fontId) ? EpdFontFamily::REGULAR : builtInStyle;
}

int dynamicShelfTitleHeight(GfxRenderer& renderer) {
  sdFontSystem.ensureLoaded(renderer);
  const int builtInHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int sdFontId = sdFontSystem.currentFontId();
  if (!renderer.isSdCardFont(sdFontId)) return builtInHeight;
  return std::max(builtInHeight, renderer.getLineHeight(sdFontId));
}

int dynamicListTitleFontId(GfxRenderer& renderer, const std::string& visibleText) {
  const int fontId = dynamicRemoteFontId(renderer, visibleText.c_str(), UI_10_FONT_ID);
  return renderer.isSdCardFont(fontId) ? fontId : 0;
}

uint32_t hashText(const std::string& text) {
  uint32_t hash = 2166136261u;
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

WeReadShelfGridLayout shelfGridLayout(GfxRenderer& renderer, const Rect& content, const int sidePadding,
                                      const int spacing) {
  WeReadShelfGridLayout layout;
  const int titleHeight = dynamicShelfTitleHeight(renderer);
  const int minimumGap = std::max(4, spacing / 2);
  const bool landscape = renderer.getOrientation() == GfxRenderer::Orientation::LandscapeClockwise ||
                         renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise;
  layout.columns = landscape ? kLandscapeShelfColumns : kPortraitShelfColumns;
  layout.rows = landscape ? kLandscapeShelfRows : kPortraitShelfRows;
  layout.itemsPerPage = layout.columns * layout.rows;
  layout.titleGap = minimumGap;
  layout.availableX = content.x + sidePadding;
  layout.availableWidth = std::max(1, content.width - sidePadding * 2);
  const int availableHeight = std::max(1, content.height);
  const int maxCoverWidth = std::max(1, (layout.availableWidth - minimumGap * (layout.columns + 1)) / layout.columns);
  const int maxCoverHeight =
      std::max(1, (availableHeight - minimumGap * (layout.rows + 1)) / layout.rows - layout.titleGap - titleHeight);
  const float widthScale = static_cast<float>(maxCoverWidth) / WeReadStore::kCoverThumbWidth;
  const float heightScale = static_cast<float>(maxCoverHeight) / WeReadStore::kCoverThumbHeight;
  const float scale = std::min(widthScale, heightScale);
  layout.coverWidth = std::max(1, static_cast<int>(WeReadStore::kCoverThumbWidth * scale));
  layout.coverHeight = std::max(1, static_cast<int>(WeReadStore::kCoverThumbHeight * scale));
  layout.itemHeight = layout.coverHeight + layout.titleGap + titleHeight;
  layout.columnGap = std::max(0, (layout.availableWidth - layout.columns * layout.coverWidth) / (layout.columns + 1));
  layout.rowGap = std::max(0, (availableHeight - layout.rows * layout.itemHeight) / (layout.rows + 1));
  return layout;
}

bool drawCachedCover(GfxRenderer& renderer, const std::string& bookDir, const Rect& bounds) {
  const std::string path = WeReadStore::coverPath(bookDir);
  if (!Storage.exists(path.c_str())) return false;

  HalFile file;
  if (!Storage.openFileForRead("WR", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  const float scale = std::min(static_cast<float>(bounds.width) / bitmap.getWidth(),
                               static_cast<float>(bounds.height) / bitmap.getHeight());
  const int width = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
  const int height = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
  renderer.drawBitmap(bitmap, bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - height) / 2,
                      bounds.width, bounds.height, 0, 0, true);
  return true;
}

void drawProgressStatus(GfxRenderer& renderer, const Rect& content, const char* title, const char* status,
                        const uint32_t completed, const uint32_t total) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int titleFontId = dynamicRemoteFontId(renderer, title, UI_12_FONT_ID);
  const auto titleStyle = dynamicRemoteFontStyle(renderer, titleFontId);
  const int titleHeight = renderer.getLineHeight(titleFontId);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int relatedGap = SubpageLayout::relatedGap(metrics);
  const int sectionGap = SubpageLayout::sectionGap(metrics);
  const int barBlockHeight =
      total > 0 ? sectionGap + GUI.measureProgressBarHeight(renderer, metrics.progressBarHeight) : 0;
  const int groupHeight = titleHeight + relatedGap + lineHeight + barBlockHeight;
  int y = content.y + std::max(0, (content.height - groupHeight) / 2);
  UITheme::drawCenteredText(renderer, content, titleFontId, y, title, true, titleStyle);
  y += titleHeight + relatedGap;
  UITheme::drawCenteredText(renderer, content, UI_10_FONT_ID, y, status);
  if (total == 0) return;

  const int sidePadding = std::min(metrics.contentSidePadding, content.width / 4);
  GUI.drawProgressBar(renderer,
                      Rect{content.x + sidePadding, y + lineHeight + sectionGap,
                           std::max(1, content.width - sidePadding * 2), metrics.progressBarHeight},
                      completed, total);
}

struct Utf8Glyph {
  char text[5] = {};
  uint8_t fileBytes = 0;
  uint8_t textBytes = 0;
};

bool readUtf8Glyph(HalFile& file, uint32_t remaining, Utf8Glyph& glyph) {
  glyph = {};
  if (remaining == 0) return false;
  const int first = file.read();
  if (first < 0) return false;
  glyph.fileBytes = 1;
  const auto lead = static_cast<uint8_t>(first);
  int expected = 1;
  if ((lead & 0xE0) == 0xC0) {
    expected = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    expected = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    expected = 4;
  }
  if (expected == 1 && lead >= 0x80) {
    glyph.text[0] = '?';
    glyph.textBytes = 1;
    return true;
  }
  glyph.text[0] = static_cast<char>(lead);
  glyph.textBytes = 1;
  for (int i = 1; i < expected; ++i) {
    if (glyph.fileBytes >= remaining) {
      glyph.text[0] = '?';
      glyph.text[1] = '\0';
      glyph.textBytes = 1;
      return true;
    }
    const int next = file.read();
    if (next < 0) return false;
    ++glyph.fileBytes;
    if ((next & 0xC0) != 0x80) {
      glyph.text[0] = '?';
      glyph.text[1] = '\0';
      glyph.textBytes = 1;
      return true;
    }
    glyph.text[glyph.textBytes++] = static_cast<char>(next);
  }
  glyph.text[glyph.textBytes] = '\0';
  return true;
}

void primeIntroAdvanceTable(GfxRenderer& renderer, const int fontId, HalFile& file, const uint32_t introLength) {
  if (!renderer.isSdCardFont(fontId) || introLength == 0) return;

  // SD font metrics are read much faster in batches than through the on-demand
  // glyph path. Keep a small overlap so a UTF-8 codepoint split at a chunk
  // boundary is seen in full by the next batch.
  auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[kIntroAdvanceChunkBytes + 4]());
  if (!buffer) {
    LOG_ERR("WR", "intro advance table buffer allocation failed");
    return;
  }
  size_t carry = 0;
  uint32_t offset = 0;
  if (!file.seek(WeReadStore::kBookDetailHeaderSize)) return;

  while (offset < introLength) {
    const size_t request = std::min<size_t>(kIntroAdvanceChunkBytes, introLength - offset);
    const int read = file.read(buffer.get() + carry, request);
    if (read <= 0) break;

    const size_t total = carry + static_cast<size_t>(read);
    buffer[total] = '\0';
    renderer.ensureSdCardFontReady(fontId, buffer.get(), /*styleMask=*/0x01);

    carry = std::min<size_t>(3, total);
    if (carry > 0) memmove(buffer.get(), buffer.get() + total - carry, carry);
    offset += static_cast<uint32_t>(read);
  }
}

void logHeap([[maybe_unused]] const char* phase) {
  LOG_DBG("WR", "%s: free=%u largest=%u stack=%u", phase, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

class WeReadChapterRangeActivity final : public Activity {
 public:
  WeReadChapterRangeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WeReadClient::Operation& operation,
                             const int chapterCount)
      : Activity("WeReadChapterRange", renderer, mappedInput), operation_(operation), chapterCount_(chapterCount) {}

  void onEnter() override {
    Activity::onEnter();
    logHeap("chapter range selector");
    requestUpdate();
  }

  void loop() override {
    if (readFailed_.load()) {
      setResult(ActivityResult{});
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const Rect content = SubpageLayout::contentRect(screen, metrics);
    const int pageItems = GUI.getListPageItems(content.height, false);
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      selectCurrent();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, chapterCount_);
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, chapterCount_);
      requestUpdate();
    });
    buttonNavigator_.onNextContinuous([this, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, chapterCount_, pageItems);
      requestUpdate();
    });
    buttonNavigator_.onPreviousContinuous([this, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, chapterCount_, pageItems);
      requestUpdate();
    });
  }

  void render(RenderLock&&) override {
    renderer.clearScreen();

    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const Rect content = SubpageLayout::contentRect(screen, metrics);
    const int pageItems = GUI.getListPageItems(content.height, false);
    GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                   I18N.get(stageTitle()));

    const int pageStart = selectedIndex_ / std::max(1, pageItems) * std::max(1, pageItems);
    std::string visibleTitles;
    for (int index = pageStart; index < chapterCount_ && index < pageStart + pageItems; ++index) {
      visibleTitles += rowTitle(index);
      visibleTitles.push_back('\n');
    }
    const int chapterTitleFontId = dynamicListTitleFontId(renderer, visibleTitles);

    GUI.drawList(
        renderer, content, chapterCount_, selectedIndex_,
        [this](const int index) { return rowTitle(index); }, nullptr, nullptr,
        [this](const int index) {
          return stage_ == Stage::End && index == firstIndex_ ? std::string(tr(STR_WEREAD_CACHE_RANGE_START_MARK))
                                                              : std::string();
        },
        false, [this](const int index) { return stage_ == Stage::End && index < firstIndex_; }, chapterTitleFontId);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }

 private:
  enum class Stage : uint8_t { Start, End };

  WeReadClient::Operation& operation_;
  ButtonNavigator buttonNavigator_;
  WeReadStore::TocRecord rowRecord_;
  std::atomic<bool> readFailed_{false};
  int chapterCount_ = 0;
  int selectedIndex_ = 0;
  int firstIndex_ = 0;
  Stage stage_ = Stage::Start;

  StrId stageTitle() const {
    switch (stage_) {
      case Stage::Start:
        return StrId::STR_WEREAD_CACHE_RANGE_START;
      case Stage::End:
        return StrId::STR_WEREAD_CACHE_RANGE_END;
    }
    return StrId::STR_WEREAD_CACHE_RANGE_START;
  }

  std::string rowTitle(const int index) {
    if (readFailed_.load()) return {};
    if (!operation_.readChapter(static_cast<uint32_t>(index), rowRecord_) ||
        !memchr(rowRecord_.title, '\0', sizeof(rowRecord_.title))) {
      readFailed_.store(true);
      return {};
    }
    char text[sizeof(rowRecord_.title) + 16];
    snprintf(text, sizeof(text), "%u. %s", static_cast<unsigned>(index + 1),
             rowRecord_.title[0] ? rowRecord_.title : tr(STR_UNNAMED));
    return text;
  }

  void selectCurrent() {
    switch (stage_) {
      case Stage::Start:
        firstIndex_ = selectedIndex_;
        stage_ = Stage::End;
        requestUpdate();
        return;
      case Stage::End:
        if (selectedIndex_ < firstIndex_) return;
        setResult(ChapterRangeResult{static_cast<uint32_t>(firstIndex_), static_cast<uint32_t>(selectedIndex_)});
        finish();
        return;
    }
  }
};

static_assert(sizeof(WeReadChapterRangeActivity) <= 1024, "WeRead chapter selector exceeds its fixed heap budget");

}  // namespace

void WeReadActivity::onEnter() {
  Activity::onEnter();
  if (!WeReadBrowse::clearLegacyWorkspace()) LOG_ERR("WR", "legacy browse workspace cleanup failed");
  NetworkStartup::prepare(renderer);
  disclaimerSelected_ = 0;
  disclaimerSaveFailed_ = false;
  manageSelected_ = 0;
  shelfSelected_.store(0);
  shelfFrameInvalidated_.store(true);
  mainTab_.store(MainTab::Shelf);
  mainFocus_.store(MainFocus::Content);
  // drawTabBar requires a vector; reserve its fixed 16-byte ESP32-C3 payload
  // once for the Activity lifetime instead of allocating in the render path.
  mainTabs_.clear();
  mainTabs_.reserve(kMainTabCount);
  mainTabs_.push_back({tr(STR_WEREAD_TAB_SHELF), true});
  mainTabs_.push_back({tr(STR_WEREAD_TAB_MANAGE), false});
  resetShelfCoverLoading();
  detailSelected_.store(0);
  detailFrameSelection_.store(-1);
  detailFrameValid_.store(false);
  detailSelectionOnlyPending_.store(false);
  detailSelectionGeneration_.store(0);
  introPage_ = 0;
  introPageCount_ = 1;
  detail_ = {};
  detailLoaded_ = false;
  detailLoadFailed_ = false;
  detailOptionsKnown_ = false;
  detailIntroTruncated_ = false;
  introPagesTruncated_ = false;
  introPreviewPrewarmHash_ = 0;
  introPreviewPrewarmLength_ = 0;
  introPreviewPrewarmFontId_ = 0;
  downloadChapterScope_ = WeReadClient::DownloadOptions::ChapterScope::WholeBook;
  optionPopupClosing_ = false;
  wifiSessionActive_ = false;
  wifiReleasePending_ = false;
  syncShelfCoverScope_ = WeReadClient::Operation::ShelfCoverScope::None;
  LOG_DBG("WR", "onEnter activity=%u free=%u largest=%u", static_cast<unsigned>(sizeof(*this)),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (!WeReadStore::hasAcceptedDisclaimer()) {
    state_.store(State::Disclaimer);
    requestUpdate();
    return;
  }
  enterApp();
}

void WeReadActivity::enterApp() {
  disclaimerSaveFailed_ = false;
  if (!WeReadStore::ensureCacheGeneration()) {
    retryJob_ = Job::Sync;
    error_ = WeReadClient::Error::SdCard;
    state_.store(State::Error);
    requestUpdate();
    return;
  }
  // This bounded 832-byte probe is gone before TLS and avoids a transient heap
  // allocation that could fragment the ESP32-C3 heap.
  WeReadStore::Session session;
  const bool loggedIn = WeReadStore::loadSession(session);
  session.clear();
  if (loggedIn) {
    openShelf();
  } else {
    syncShelf();
  }
}

void WeReadActivity::onExit() {
  operation_.reset();
  downloadRenderPending_.store(false);
  stageRenderPending_.store(false);
  if (shelfFile_.isOpen()) shelfFile_.close();
  std::vector<TabInfo>().swap(mainTabs_);
  if (wifiSessionActive_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    esp_wifi_deinit();
  }
  LOG_DBG("WR", "onExit free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  Activity::onExit();
}

bool WeReadActivity::refreshShelf() {
  shelfFrameInvalidated_.store(true);
  if (shelfFile_.isOpen()) shelfFile_.close();
  shelfCount_ = 0;
  if (!WeReadStore::openShelf(shelfFile_, shelfCount_)) {
    if (shelfFile_.isOpen()) shelfFile_.close();
    return false;
  }
  if (shelfCount_ == 0) {
    shelfSelected_.store(0);
  } else if (shelfSelected_.load() >= static_cast<int>(shelfCount_)) {
    shelfSelected_.store(static_cast<int>(shelfCount_ - 1));
  }
  resetShelfCoverLoading();
  return true;
}

bool WeReadActivity::readShelf(const int index, WeReadStore::ShelfRecord& record) const {
  return index >= 0 && static_cast<uint32_t>(index) < shelfCount_ &&
         WeReadStore::readShelfRecord(shelfFile_, static_cast<uint32_t>(index), record);
}

Rect WeReadActivity::contentBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return SubpageLayout::contentRect(safe, metrics);
}

Rect WeReadActivity::mainContentBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return SubpageLayout::contentRect(UITheme::getInstance().getScreenSafeArea(renderer, true, false), metrics, true);
}

Rect WeReadActivity::detailActionsBounds(const Rect& content) const {
  const int height = kDetailListActionCount * GUI.getListRowStep(false);
  return Rect{content.x, content.y + content.height - height, content.width, height};
}

Rect WeReadActivity::detailIntroductionBounds(const Rect& content) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect actions = detailActionsBounds(content);
  const int y = content.y + kDetailCoverHeight + metrics.verticalSpacing;
  return Rect{content.x + metrics.contentSidePadding, y, content.width - metrics.contentSidePadding * 2,
              std::max(1, actions.y - metrics.verticalSpacing - y)};
}

Rect WeReadActivity::disclaimerSafeBounds() const {
  return UITheme::getInstance().getScreenSafeArea(renderer, true, false);
}

Rect WeReadActivity::disclaimerContentBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return SubpageLayout::contentRect(disclaimerSafeBounds(), metrics);
}

Rect WeReadActivity::disclaimerActionsBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = disclaimerContentBounds();
  const int minimumHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const int height = std::min(content.height, std::max(GUI.getListRowStep(false), minimumHeight));
  const int availableWidth = std::max(0, content.width - metrics.contentSidePadding * 2);
  const int labelWidth =
      std::accumulate(std::begin(kDisclaimerActions), std::end(kDisclaimerActions), 0, [this](int width, StrId action) {
        return std::max(width, renderer.getTextWidth(UI_10_FONT_ID, I18N.get(action)));
      });
  const int gap = disclaimerActionGap(availableWidth, metrics.verticalSpacing);
  const int targetWidth =
      (labelWidth + metrics.contentSidePadding * 2) * kDisclaimerActionCount + gap * (kDisclaimerActionCount - 1);
  const int width = std::min(availableWidth, targetWidth);
  const int bottomY = content.y + content.height - height;
  const int cachedY = disclaimerActionsY_.load();
  const int y = cachedY >= content.y ? std::min(cachedY, bottomY) : bottomY;
  return Rect{content.x + (content.width - width) / 2, y, width, height};
}

int WeReadActivity::shelfItemsPerPage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return shelfGridLayout(renderer, mainContentBounds(), metrics.contentSidePadding, metrics.verticalSpacing)
      .itemsPerPage;
}

void WeReadActivity::resetShelfCoverLoading() {
  if (state_.load() == State::Home) operation_.reset();
  shelfCoverPageStart_ = -1;
  shelfCoverCursor_ = 0;
  shelfCoverStopped_ = false;
}

void WeReadActivity::requestDownloadUpdate() {
  if (!downloadRenderPending_.exchange(true)) requestUpdate();
}

void WeReadActivity::requestJobUpdate() {
  if (retryJob_ == Job::Download) {
    requestDownloadUpdate();
  } else {
    requestUpdate();
  }
}

WeReadActivity::State WeReadActivity::stateForJob(const Job job) {
  switch (job) {
    case Job::Sync:
      return State::Syncing;
    case Job::Detail:
      return State::DetailLoading;
    case Job::Download:
      return State::Downloading;
  }
  return State::Error;
}

bool isPostProcessStage(const WeReadClient::Operation::ProgressStage stage) {
  switch (stage) {
    case WeReadClient::Operation::ProgressStage::Preparing:
    case WeReadClient::Operation::ProgressStage::Packaging:
      return true;
    case WeReadClient::Operation::ProgressStage::Chapters:
    case WeReadClient::Operation::ProgressStage::Images:
      return false;
  }
  return false;
}

void WeReadActivity::connectThen(const Job job, const WeReadStore::ShelfRecord* book) {
  retryJob_ = job;
  if (book) pendingBook_ = *book;
  const bool syncClockOnReconnect = error_ == WeReadClient::Error::Clock;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    LOG_ERR("WR", "OOM: Wi-Fi activity");
    error_ = WeReadClient::Error::OutOfMemory;
    state_.store(State::Error);
    requestJobUpdate();
    return;
  }
  wifiSessionActive_ = true;
  startActivityForResult(std::move(wifi), [this, job, syncClockOnReconnect](const ActivityResult& result) {
    wifiReleasePending_ = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                          mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                          mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
                          mappedInput.isPressed(MappedInputManager::Button::NavNext);
    if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
      if (syncClockOnReconnect && !syncClockForRetry()) {
        state_.store(State::Error);
        requestJobUpdate();
        return;
      }
      startJob(job, job == Job::Sync ? nullptr : &pendingBook_);
      return;
    }
    error_ = WeReadClient::Error::Network;
    state_.store(State::Error);
    requestJobUpdate();
  });
}

void WeReadActivity::startJob(const Job job, const WeReadStore::ShelfRecord* book) {
  wifiSessionActive_ = true;
  if (job == Job::Sync && shelfFile_.isOpen()) shelfFile_.close();
  if (job != Job::Sync && book) pendingBook_ = *book;
  retryJob_ = job;
  error_ = WeReadClient::Error::Ok;
  progressStage_.store(WeReadClient::Operation::ProgressStage::Chapters);
  progressCompleted_.store(0);
  progressTotal_.store(0);
  postProcessNotice_.store(PostProcessNotice::None);
  postProcessStartedAt_ = 0;
  downloadRenderPending_.store(false);
  stageRenderPending_.store(job == Job::Download);
  qrUrl_[0] = '\0';
  WeReadClient::Operation::Kind kind = WeReadClient::Operation::Kind::Sync;
  switch (job) {
    case Job::Sync:
      kind = WeReadClient::Operation::Kind::Sync;
      break;
    case Job::Detail:
      kind = WeReadClient::Operation::Kind::Detail;
      break;
    case Job::Download:
      kind = WeReadClient::Operation::Kind::Download;
      break;
  }
  WeReadClient::DownloadOptions options;
  if (job == Job::Download && book) {
    options.imagePolicy = detailImagePolicy_;
    options.chapterScope = downloadChapterScope_;
  }
  if (!operation_.begin(kind, book, options, syncShelfCoverScope_)) {
    error_ = operation_.error();
    state_.store(State::Error);
    requestJobUpdate();
  } else {
    const State nextState = job == Job::Detail && detailLoaded_ ? State::DetailCoverLoading : stateForJob(job);
    state_.store(job == Job::Sync ? State::Connecting : nextState);
    if (job == Job::Sync) {
      requestUpdateAndWait();
    } else if (job == Job::Download) {
      requestDownloadUpdate();
    } else if (job == Job::Detail) {
      requestUpdateAndWait();
    }
  }
}

WeReadClient::Operation::Event WeReadActivity::stepOperation() {
  // Rendering owns a second 8KB task and large display buffers. Serialize it
  // with each synchronous protocol step so TLS never competes with a refresh.
  RenderLock renderBarrier(*this);
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->clearCache();
    introPreviewPrewarmHash_ = 0;
    introPreviewPrewarmLength_ = 0;
    introPreviewPrewarmFontId_ = 0;
  }
  // Cover conversion claims JPEGDEC from the lent 48KB framebuffer.
  struct WorkContext {
    WeReadActivity* activity;
    RenderLock* renderBarrier;
  } context{this, &renderBarrier};
  return operation_.step(
      [](void* rawContext) {
        auto* work = static_cast<WorkContext*>(rawContext);
        work->activity->maybeShowLongWait(*work->renderBarrier);
      },
      &context);
}

void WeReadActivity::updatePostProcessNotice(const WeReadClient::Operation::ProgressStage previous,
                                             const WeReadClient::Operation::ProgressStage current) {
  const bool wasPostProcessing = isPostProcessStage(previous);
  const bool isPostProcessing = isPostProcessStage(current);
  if (wasPostProcessing == isPostProcessing) return;
  if (isPostProcessing) {
    postProcessStartedAt_ = millis();
    postProcessNotice_.store(PostProcessNotice::Waiting);
  } else {
    postProcessStartedAt_ = 0;
    postProcessNotice_.store(PostProcessNotice::None);
  }
}

void WeReadActivity::maybeShowLongWait(RenderLock& renderBarrier) {
  if (postProcessNotice_.load() != PostProcessNotice::Waiting || millis() - postProcessStartedAt_ < kLongWaitMs) {
    return;
  }
  postProcessNotice_.store(PostProcessNotice::LongWait);
  renderBarrier.unlock();
  requestUpdateAndWait();
}

void WeReadActivity::advanceShelfCovers() {
  if (shelfCoverStopped_ || shelfCount_ == 0 || WiFi.status() != WL_CONNECTED) return;
  const int itemsPerPage = shelfItemsPerPage();
  const int pageStart = shelfSelected_.load() / itemsPerPage * itemsPerPage;
  if (pageStart != shelfCoverPageStart_) {
    operation_.reset();
    shelfCoverPageStart_ = pageStart;
    shelfCoverCursor_ = pageStart;
  }

  if (operation_.active()) {
    switch (stepOperation()) {
      case WeReadClient::Operation::Event::None:
      case WeReadClient::Operation::Event::DetailReady:
        return;
      case WeReadClient::Operation::Event::Complete:
        ++shelfCoverCursor_;
        shelfFrameInvalidated_.store(true);
        requestUpdate();
        break;
      case WeReadClient::Operation::Event::QrReady:
      case WeReadClient::Operation::Event::Authenticated:
      case WeReadClient::Operation::Event::ChapterRangeReady:
      case WeReadClient::Operation::Event::ChapterComplete:
      case WeReadClient::Operation::Event::Cancelled:
      case WeReadClient::Operation::Event::Failed:
        operation_.reset();
        shelfCoverStopped_ = true;
        return;
    }
  }

  const int pageEnd = std::min(pageStart + itemsPerPage, static_cast<int>(std::min<uint32_t>(shelfCount_, INT32_MAX)));
  while (shelfCoverCursor_ < pageEnd) {
    WeReadStore::ShelfRecord book;
    if (!readShelf(shelfCoverCursor_, book)) {
      shelfCoverStopped_ = true;
      return;
    }
    if (WeReadStore::coverCacheValid(WeReadStore::bookDirectory(book.bookId))) {
      ++shelfCoverCursor_;
      continue;
    }
    if (!operation_.begin(WeReadClient::Operation::Kind::Detail, &book)) {
      operation_.reset();
      shelfCoverStopped_ = true;
    }
    return;
  }
}

void WeReadActivity::updateJobProgress() {
  switch (retryJob_) {
    case Job::Detail:
      return;
    case Job::Sync:
    case Job::Download:
      break;
  }

  const auto stage = operation_.progressStage();
  const uint32_t completed = operation_.progressCompleted();
  const uint32_t total = operation_.progressTotal();
  const auto previousStage = progressStage_.exchange(stage);
  const uint32_t previousCompleted = progressCompleted_.exchange(completed);
  const uint32_t previousTotal = progressTotal_.exchange(total);
  const bool stageChanged = previousStage != stage;
  const bool totalChanged = previousTotal != total;
  const bool completedChanged = previousCompleted != completed;
  const bool decileChanged = WeReadClient::Operation::progressDecile(previousCompleted, total) !=
                             WeReadClient::Operation::progressDecile(completed, total);

  if (retryJob_ == Job::Sync && (stage != WeReadClient::Operation::ProgressStage::Chapters || completed > 0)) {
    state_.store(State::Syncing);
  }
  if (stageChanged && retryJob_ == Job::Download) {
    updatePostProcessNotice(previousStage, stage);
    stageRenderPending_.store(true);
  }

  bool requestRender = stageChanged || totalChanged;
  if (!requestRender && completedChanged) {
    switch (retryJob_) {
      case Job::Sync:
        requestRender = total == 0 ? completed > 0 : decileChanged || completed == total;
        break;
      case Job::Download:
        switch (stage) {
          case WeReadClient::Operation::ProgressStage::Chapters:
            requestRender = true;
            break;
          case WeReadClient::Operation::ProgressStage::Images:
            requestRender = decileChanged || completed == total;
            break;
          case WeReadClient::Operation::ProgressStage::Preparing:
          case WeReadClient::Operation::ProgressStage::Packaging:
            requestRender = completed == total;
            break;
        }
        break;
      case Job::Detail:
        break;
    }
  }
  if (requestRender) requestJobUpdate();
}

void WeReadActivity::advanceJob() {
  const auto event = stepOperation();
  updateJobProgress();

  switch (event) {
    case WeReadClient::Operation::Event::None:
      return;
    case WeReadClient::Operation::Event::QrReady:
      strncpy(qrUrl_, operation_.qrUrl(), sizeof(qrUrl_) - 1);
      qrUrl_[sizeof(qrUrl_) - 1] = '\0';
      state_.store(State::Qr);
      requestJobUpdate();
      return;
    case WeReadClient::Operation::Event::Authenticated:
      state_.store(State::LoginConfirmed);
      return;
    case WeReadClient::Operation::Event::DetailReady:
      if (!detailLoaded_) {
        detailLoadFailed_ = false;
        loadSelectedDetail();
        stageRenderPending_.store(true);
        state_.store(State::DetailCoverLoading);
        requestUpdate();
      }
      return;
    case WeReadClient::Operation::Event::ChapterRangeReady:
      selectChapterRange();
      return;
    case WeReadClient::Operation::Event::ChapterComplete:
      state_.store(State::Downloading);
      return;
    case WeReadClient::Operation::Event::Complete:
      switch (retryJob_) {
        case Job::Sync:
          refreshShelf();
          shelfCoverStopped_ = false;
          mainTab_.store(MainTab::Shelf);
          mainFocus_.store(MainFocus::Content);
          state_.store(State::Home);
          advanceShelfCovers();
          requestJobUpdate();
          return;
        case Job::Detail: {
          const bool preserveUi = state_.load() == State::DetailCoverLoading;
          detailLoadFailed_ = false;
          loadSelectedDetail(preserveUi);
        }
          state_.store(State::Detail);
          requestUpdate();
          return;
        case Job::Download:
          state_.store(State::OpenBook);
          openBook(operation_.finalPath());
          return;
      }
      return;
    case WeReadClient::Operation::Event::Cancelled:
      refreshShelf();
      shelfCoverStopped_ = false;
      state_.store(State::Home);
      requestJobUpdate();
      return;
    case WeReadClient::Operation::Event::Failed:
      if (retryJob_ == Job::Detail) {
        const bool preserveUi = state_.load() == State::DetailCoverLoading;
        detailLoadFailed_ = true;
        loadSelectedDetail(preserveUi);
        state_.store(State::Detail);
        requestUpdate();
        return;
      }
      error_ = operation_.error();
      refreshShelf();
      state_.store(State::Error);
      requestJobUpdate();
      return;
  }
}

void WeReadActivity::loadSelectedDetail(const bool preserveUi) {
  const int previousSelection = detailSelected_.load();
  const auto previousImagePolicy = detailImagePolicy_;
  detail_ = {};
  introPage_ = 0;
  introPageCount_ = 1;
  introPageOffsets_[0] = 0;
  introPageOffsets_[1] = 0;
  introFontId_ = UI_10_FONT_ID;
  introPreviewPrewarmHash_ = 0;
  introPreviewPrewarmLength_ = 0;
  introPreviewPrewarmFontId_ = 0;
  detailFrameSelection_.store(-1);
  detailFrameValid_.store(false);
  detailSelectionOnlyPending_.store(false);
  detailSelectionGeneration_.store(0);
  introPagesTruncated_ = false;
  memcpy(detail_.title, pendingBook_.title, sizeof(detail_.title));
  detail_.title[sizeof(detail_.title) - 1] = '\0';
  memcpy(detail_.author, pendingBook_.author, sizeof(detail_.author));
  detail_.author[sizeof(detail_.author) - 1] = '\0';
  detailIntroTruncated_ = false;

  const std::string bookDir = WeReadStore::bookDirectory(pendingBook_.bookId);
  HalFile file;
  WeReadStore::BookDetailHeader cachedDetail;
  detailLoaded_ = WeReadStore::openBookDetail(bookDir, cachedDetail, file);
  if (detailLoaded_) detail_ = cachedDetail;

  WeReadStore::BookOptions options;
  detailOptionsKnown_ = WeReadStore::loadBookOptions(bookDir, options);
  detailSavedImagePolicy_ = options.imagePolicy;
  detailImagePolicy_ = preserveUi ? previousImagePolicy : detailSavedImagePolicy_;
  if (detail_.introLength > 0) buildIntroductionPages();
  const bool cached = Storage.exists(WeReadStore::finalBookPath(pendingBook_).c_str());
  detailSelected_.store(
      preserveUi ? previousSelection : static_cast<int>(cached ? DetailAction::Read : DetailAction::Cache));
}

void WeReadActivity::openSelectedDetail(const WeReadStore::ShelfRecord& book) {
  pendingBook_ = book;
  detailLoadFailed_ = false;
  loadSelectedDetail();
  const std::string bookDir = WeReadStore::bookDirectory(book.bookId);
  char coverSource[128];
  int coverSourceLength = snprintf(coverSource, sizeof(coverSource), "%s/cover.source.png", bookDir.c_str());
  bool coverSourceMissing = coverSourceLength <= 0 || static_cast<size_t>(coverSourceLength) >= sizeof(coverSource) ||
                            !Storage.exists(coverSource);
  coverSourceLength = snprintf(coverSource, sizeof(coverSource), "%s/cover.source.jpg", bookDir.c_str());
  coverSourceMissing =
      coverSourceMissing && (coverSourceLength <= 0 || static_cast<size_t>(coverSourceLength) >= sizeof(coverSource) ||
                             !Storage.exists(coverSource));
  const bool coverMissing = detailLoaded_ && detail_.coverUrl[0] &&
                            (!Storage.exists(WeReadStore::coverPath(bookDir).c_str()) || coverSourceMissing);
  if (!detailLoaded_) {
    if (WiFi.status() == WL_CONNECTED) {
      startJob(Job::Detail, &book);
    } else {
      state_.store(State::DetailLoading);
      requestUpdateAndWait();
      connectThen(Job::Detail, &book);
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED && coverMissing) {
    startJob(Job::Detail, &book);
    return;
  }
  state_.store(State::Detail);
  requestUpdate();
}

void WeReadActivity::activateSelected() {
  WeReadStore::ShelfRecord book;
  if (readShelf(shelfSelected_.load(), book)) openSelectedDetail(book);
}

bool WeReadActivity::detailActionEnabled(const DetailAction action) const {
  switch (action) {
    case DetailAction::Introduction:
      return detailIntroTruncated_;
    case DetailAction::Read:
      return Storage.exists(WeReadStore::finalBookPath(pendingBook_).c_str());
    case DetailAction::Cache:
    case DetailAction::Browse:
    case DetailAction::Images:
      return true;
  }
  return false;
}

void WeReadActivity::moveDetailSelection(const int direction) {
  int selection = detailSelected_.load();
  for (int i = 0; i < kDetailActionCount; ++i) {
    selection = direction > 0 ? ButtonNavigator::nextIndex(selection, kDetailActionCount)
                              : ButtonNavigator::previousIndex(selection, kDetailActionCount);
    if (detailActionEnabled(static_cast<DetailAction>(selection))) {
      detailSelected_.store(selection);
      return;
    }
  }
  detailSelected_.store(selection);
}

void WeReadActivity::activateDetailSelection() {
  const auto action = static_cast<DetailAction>(detailSelected_.load());
  if (!detailActionEnabled(action)) return;
  detailSelectionOnlyPending_.store(false);
  // Every activation either leaves the detail page or changes something that
  // must be rendered again. Do not let a queued notification reuse the old
  // detail framebuffer as if it were still the current screen.
  detailFrameValid_.store(false);
  switch (action) {
    case DetailAction::Introduction:
      buildIntroductionPages();
      introPage_ = 0;
      state_.store(State::Introduction);
      requestUpdate();
      return;
    case DetailAction::Read: {
      const std::string finalPath = WeReadStore::finalBookPath(pendingBook_);
      if (Storage.exists(finalPath.c_str())) {
        openBook(finalPath.c_str());
        return;
      }
      return;
    }
    case DetailAction::Cache:
      showCacheScopePopup();
      return;
    case DetailAction::Browse: {
      // The Activity stack requires heap ownership; the child keeps only a
      // reference to this Activity's fixed Operation workspace.
      auto browse = makeUniqueNoThrow<WeReadBrowseActivity>(renderer, mappedInput, operation_, pendingBook_);
      if (!browse) {
        LOG_ERR("WR", "OOM: browse activity (%zu bytes)", sizeof(WeReadBrowseActivity));
        error_ = WeReadClient::Error::OutOfMemory;
        state_.store(State::Error);
        requestUpdate();
        return;
      }
      startActivityForResult(std::move(browse), [this](const ActivityResult&) {
        if (WiFi.getMode() != WIFI_MODE_NULL) wifiSessionActive_ = true;
      });
      return;
    }
    case DetailAction::Images:
      detailImagePolicy_ = detailImagePolicy_ == WeReadStore::ImagePolicy::Embed ? WeReadStore::ImagePolicy::Exclude
                                                                                 : WeReadStore::ImagePolicy::Embed;
      requestUpdate();
      return;
  }
}

void WeReadActivity::showCacheScopePopup() {
  optionPopup_.show(StrId::STR_WEREAD_CACHE_BOOK, kCacheScopeOptions,
                    static_cast<int>(sizeof(kCacheScopeOptions) / sizeof(kCacheScopeOptions[0])), 0,
                    [this](const int index) {
                      const auto scope = static_cast<WeReadClient::DownloadOptions::ChapterScope>(index);
                      switch (scope) {
                        case WeReadClient::DownloadOptions::ChapterScope::WholeBook:
                        case WeReadClient::DownloadOptions::ChapterScope::SelectRange:
                          downloadChapterScope_ = scope;
                          startBookDownload();
                          return;
                      }
                    });
  requestUpdate();
}

void WeReadActivity::showShelfRefreshPopup() {
  optionPopup_.show(StrId::STR_WEREAD_CACHE_ALL_COVERS_CONFIRM, kShelfRefreshOptions,
                    static_cast<int>(sizeof(kShelfRefreshOptions) / sizeof(kShelfRefreshOptions[0])), 0,
                    [this](const int index) {
                      syncShelf(index == 0 ? WeReadClient::Operation::ShelfCoverScope::None
                                           : WeReadClient::Operation::ShelfCoverScope::All);
                    });
  requestUpdate();
}

void WeReadActivity::startBookDownload() {
  if (WiFi.status() == WL_CONNECTED) {
    startJob(Job::Download, &pendingBook_);
  } else {
    connectThen(Job::Download, &pendingBook_);
  }
}

void WeReadActivity::failChapterRangeSelection(const WeReadClient::Error error) {
  operation_.reset();
  error_ = error;
  state_.store(State::Error);
  requestDownloadUpdate();
}

void WeReadActivity::cancelChapterRangeSelection() {
  operation_.reset();
  downloadChapterScope_ = WeReadClient::DownloadOptions::ChapterScope::WholeBook;
  detailFrameValid_.store(false);
  state_.store(State::Detail);
  requestUpdate();
}

void WeReadActivity::selectChapterRange() {
  const uint32_t chapterCount = operation_.chapterCount();
  if (chapterCount == 0 || chapterCount > static_cast<uint32_t>(INT_MAX)) {
    failChapterRangeSelection(WeReadClient::Error::Protocol);
    return;
  }

  // The Activity stack requires heap ownership. One bounded selector retains a
  // single TocRecord; chapter titles remain in the SD-backed index.
  auto selector =
      makeUniqueNoThrow<WeReadChapterRangeActivity>(renderer, mappedInput, operation_, static_cast<int>(chapterCount));
  if (!selector) {
    LOG_ERR("WR", "OOM: chapter range selector (%zu bytes)", sizeof(WeReadChapterRangeActivity));
    failChapterRangeSelection(WeReadClient::Error::OutOfMemory);
    return;
  }

  startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      cancelChapterRangeSelection();
      return;
    }
    const auto* range = std::get_if<ChapterRangeResult>(&result.data);
    if (!range) {
      failChapterRangeSelection(WeReadClient::Error::SdCard);
      return;
    }
    if (!operation_.setChapterRange(range->first, range->last)) {
      failChapterRangeSelection(WeReadClient::Error::Protocol);
      return;
    }
    progressStage_.store(WeReadClient::Operation::ProgressStage::Chapters);
    progressCompleted_.store(0);
    progressTotal_.store(range->last - range->first + 1);
    stageRenderPending_.store(true);
    state_.store(State::Downloading);
    requestDownloadUpdate();
  });
}

void WeReadActivity::handleDetailInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state_.load() == State::DetailCoverLoading) operation_.reset();
    detailSelectionOnlyPending_.store(false);
    detailFrameValid_.store(false);
    mainTab_.store(MainTab::Shelf);
    mainFocus_.store(MainFocus::Content);
    state_.store(State::Home);
    requestUpdate();
    return;
  }

  buttonNavigator_.onNext([this] {
    const int previousSelection = detailSelected_.load();
    moveDetailSelection(1);
    if (previousSelection != detailSelected_.load()) {
      detailSelectionGeneration_.fetch_add(1);
      detailSelectionOnlyPending_.store(true);
      requestUpdate();
    }
  });
  buttonNavigator_.onPrevious([this] {
    const int previousSelection = detailSelected_.load();
    moveDetailSelection(-1);
    if (previousSelection != detailSelected_.load()) {
      detailSelectionGeneration_.fetch_add(1);
      detailSelectionOnlyPending_.store(true);
      requestUpdate();
    }
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateDetailSelection();
  }
}

void WeReadActivity::handleIntroductionInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state_.store(retryJob_ == Job::Detail && operation_.active() ? State::DetailCoverLoading : State::Detail);
    requestUpdate();
    return;
  }

  const auto next = [this] {
    if (introPage_ + 1 < introPageCount_) {
      ++introPage_;
      requestUpdate();
    }
  };
  const auto previous = [this] {
    if (introPage_ > 0) {
      --introPage_;
      requestUpdate();
    }
  };
  buttonNavigator_.onNext(next);
  buttonNavigator_.onPrevious(previous);
}

void WeReadActivity::buildIntroductionPages() {
  introPage_ = 0;
  introPageCount_ = 1;
  introPagesTruncated_ = false;
  introPageOffsets_[0] = 0;
  introPageOffsets_[1] = detail_.introLength;
  introPreviewLineCount_ = 0;
  introFontId_ = UI_10_FONT_ID;
  introPreviewPrewarmHash_ = 0;
  introPreviewPrewarmLength_ = 0;
  introPreviewPrewarmFontId_ = 0;
  if (!detail_.introLength) return;

  HalFile file;
  WeReadStore::BookDetailHeader header;
  if (!WeReadStore::openBookDetail(WeReadStore::bookDirectory(pendingBook_.bookId), header, file)) return;
  // The introduction is remote book content rather than fixed UI text. Use the
  // selected SD font for the whole body so Chinese glyphs, line metrics and
  // pagination are always handled by the same font. Fixed labels remain built-in.
  introFontId_ = dynamicRemoteBodyFontId(renderer, UI_10_FONT_ID, "paginate");
  primeIntroAdvanceTable(renderer, introFontId_, file, header.introLength);
  if (!file.seek(WeReadStore::kBookDetailHeaderSize)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentHeight = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight -
                            metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int lineHeight = renderer.getLineHeight(introFontId_);
  const int linesPerPage = std::max(1, (contentHeight - lineHeight - metrics.verticalSpacing) / lineHeight);
  const int maxWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  LOG_DBG("WR", "intro pagination: book=%s bytes=%u fontId=%d isSd=%u lineHeight=%d lines=%d width=%d",
          pendingBook_.bookId, static_cast<unsigned>(header.introLength), introFontId_,
          renderer.isSdCardFont(introFontId_) ? 1U : 0U, lineHeight, linesPerPage, maxWidth);

  uint32_t offset = 0;
  int line = 1;
  int lineWidth = 0;
  size_t lineBytes = 0;
  uint32_t lineStart = 0;
  uint32_t lineContentEnd = 0;
  const auto recordPreviewLine = [this](const uint32_t start, const uint32_t end, const int width) {
    if (introPreviewLineCount_ >= kMaxIntroPreviewLines) return;
    introPreviewLineStarts_[introPreviewLineCount_] = start;
    introPreviewLineEnds_[introPreviewLineCount_] = end;
    introPreviewLineWidths_[introPreviewLineCount_] = static_cast<uint16_t>(std::clamp(width, 0, 65535));
    ++introPreviewLineCount_;
  };
  while (offset < header.introLength) {
    const uint32_t glyphStart = offset;
    Utf8Glyph glyph;
    if (!readUtf8Glyph(file, header.introLength - offset, glyph)) break;
    offset += glyph.fileBytes;
    if (glyph.textBytes == 1 && glyph.text[0] == '\r') continue;
    if (glyph.textBytes == 1 && glyph.text[0] == '\n') {
      if (line <= linesPerPage) recordPreviewLine(lineStart, lineContentEnd, lineWidth);
      ++line;
      lineStart = offset;
      lineContentEnd = offset;
      lineBytes = 0;
      lineWidth = 0;
      if (line > linesPerPage) {
        if (introPageCount_ >= kMaxIntroPages) {
          introPagesTruncated_ = true;
          break;
        }
        introPageOffsets_[introPageCount_++] = offset;
        line = 1;
        lineStart = offset;
      }
      continue;
    }

    const int glyphWidth = renderer.getTextAdvanceX(introFontId_, glyph.text, EpdFontFamily::REGULAR);
    if ((lineWidth > 0 && lineWidth + glyphWidth > maxWidth) ||
        lineBytes + glyph.textBytes >= kIntroLineBufferBytes) {
      if (line <= linesPerPage) recordPreviewLine(lineStart, glyphStart, lineWidth);
      ++line;
      lineStart = glyphStart;
      lineContentEnd = glyphStart;
      lineBytes = 0;
      lineWidth = 0;
      if (line > linesPerPage) {
        if (introPageCount_ >= kMaxIntroPages) {
          introPagesTruncated_ = true;
          break;
        }
        introPageOffsets_[introPageCount_++] = glyphStart;
        line = 1;
        lineStart = glyphStart;
      }
    }
    lineWidth += glyphWidth;
    lineBytes += glyph.textBytes;
    lineContentEnd = offset;
  }
  if (!introPagesTruncated_ && line <= linesPerPage && lineContentEnd > lineStart) {
    recordPreviewLine(lineStart, lineContentEnd, lineWidth);
  }
  introPageOffsets_[introPageCount_] = introPagesTruncated_ ? offset : header.introLength;
}

void WeReadActivity::openBook(const char* path) {
  logHeap("open reader");
#ifdef CROSSPOINT_EMULATED
  activityManager.goToReader(path);
#else
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    activityManager.goToReader(path);
    return;
  }

  APP_STATE.openEpubPath = path;
  APP_STATE.readerActivityLoadCount = 0;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("WR", "Failed to persist reader target; opening without restart");
    activityManager.goToReader(path);
    return;
  }

  WiFi.disconnect(false);
  delay(30);
  silentRestartToReader();
#endif
}

void WeReadActivity::openShelf() {
  if (refreshShelf()) {
    state_.store(State::Home);
    requestUpdate();
    return;
  }
  syncShelf();
}

void WeReadActivity::syncShelf(const WeReadClient::Operation::ShelfCoverScope scope) {
  syncShelfCoverScope_ = scope;
  if (WiFi.status() == WL_CONNECTED) {
    startJob(Job::Sync);
  } else {
    connectThen(Job::Sync);
  }
}

void WeReadActivity::activateDisclaimerSelection() {
  if (disclaimerSelected_ == 0) {
    activityManager.goToExtensions();
    return;
  }
  if (!WeReadStore::acceptDisclaimer()) {
    LOG_ERR("WR", "Failed to persist disclaimer acceptance");
    disclaimerSaveFailed_ = true;
    requestUpdate();
    return;
  }
  enterApp();
}

void WeReadActivity::handleDisclaimerInput() {
  buttonNavigator_.onNextRelease([this] {
    disclaimerSelected_ = ButtonNavigator::nextIndex(disclaimerSelected_, kDisclaimerActionCount);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this] {
    disclaimerSelected_ = ButtonNavigator::previousIndex(disclaimerSelected_, kDisclaimerActionCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateDisclaimerSelection();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToExtensions();
  }
}

void WeReadActivity::promptLogout() {
  // ActivityManager owns the confirmation across frames, so this must be a
  // fallible heap allocation rather than a stack object.
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_WEREAD_LOGOUT_CONFIRM),
                                                              tr(STR_WEREAD_LOGOUT_KEEP_DOWNLOADS));
  if (!confirmation) {
    LOG_ERR("WR", "OOM: logout confirmation (%zu bytes)", sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    performLogout();
  });
}

void WeReadActivity::promptClearCache() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_WEREAD_MENU_CLEAR_CACHE),
                                                              tr(STR_WEREAD_CLEAR_CACHE_KEEP_BOOKS));
  if (!confirmation) {
    LOG_ERR("WR", "OOM: clear cache confirmation (%zu bytes)", sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    performClearCache();
  });
}

void WeReadActivity::performClearCache() {
  operation_.reset();
  if (shelfFile_.isOpen()) shelfFile_.close();
  state_.store(State::ClearingCache);
  requestUpdateAndWait();

  const bool cleared = WeReadStore::clearCache();
  refreshShelf();
  resetShelfCoverLoading();
  detail_ = {};
  detailLoaded_ = false;
  detailLoadFailed_ = false;
  detailOptionsKnown_ = false;
  detailIntroTruncated_ = false;
  introPage_ = 0;
  introPageCount_ = 1;
  state_.store(cleared ? State::CacheCleared : State::CacheClearError);
  requestUpdate();
}

void WeReadActivity::performLogout() {
  operation_.reset();
  if (shelfFile_.isOpen()) shelfFile_.close();
  const bool sessionCleared = WeReadStore::clearSession();
  const bool shelfCleared = WeReadStore::clearShelf();
  const bool browseCacheCleared = WeReadBrowse::clearAllCaches();
  shelfCount_ = 0;
  shelfSelected_.store(0);
  shelfFrameInvalidated_.store(true);
  if (!sessionCleared || !shelfCleared || !browseCacheCleared) {
    LOG_ERR("WR", "Failed to clear local login state");
    state_.store(State::LogoutError);
    requestUpdate();
    return;
  }
  mainTab_.store(MainTab::Shelf);
  mainFocus_.store(MainFocus::Content);
  syncShelf();
}

void WeReadActivity::selectMainTab(const MainTab tab) {
  if (mainTab_.load() == tab) return;
  resetShelfCoverLoading();
  mainTab_.store(tab);
  requestUpdate();
}

void WeReadActivity::handleMainTabInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    mainFocus_.store(MainFocus::Content);
    requestUpdate();
    return;
  }

  const bool swapFrontDirections = mappedInput.isNavDirectionSwapped();
  const auto previousButton =
      swapFrontDirections ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFrontDirections ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  if (!mappedInput.wasReleased(previousButton) && !mappedInput.wasReleased(nextButton)) return;

  selectMainTab(mainTab_.load() == MainTab::Shelf ? MainTab::Manage : MainTab::Shelf);
}

void WeReadActivity::handleManageInput() {
  const auto activate = [this] {
    switch (kManageEntries[manageSelected_].action) {
      case ManageAction::Refresh:
        showShelfRefreshPopup();
        return;
      case ManageAction::ClearCache:
        promptClearCache();
        return;
      case ManageAction::Logout:
        promptLogout();
        return;
    }
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (manageSelected_ == 0) {
      mainFocus_.store(MainFocus::Tabs);
    } else {
      --manageSelected_;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    manageSelected_ = ButtonNavigator::nextIndex(manageSelected_, kManageEntryCount);
    requestUpdate();
    return;
  }

  const bool swapFrontDirections = mappedInput.isNavDirectionSwapped();
  const auto previousButton =
      swapFrontDirections ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFrontDirections ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  if (mappedInput.wasReleased(previousButton)) {
    if (manageSelected_ == 0) {
      mainFocus_.store(MainFocus::Tabs);
    } else {
      --manageSelected_;
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(nextButton)) {
    manageSelected_ = ButtonNavigator::nextIndex(manageSelected_, kManageEntryCount);
    requestUpdate();
  }
}

void WeReadActivity::handleMainInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    resetShelfCoverLoading();
    activityManager.goToExtensions();
    return;
  }

  if (mainFocus_.load() == MainFocus::Tabs) {
    handleMainTabInput();
    return;
  }

  switch (mainTab_.load()) {
    case MainTab::Shelf:
      handleShelfInput();
      if (state_.load() == State::Home && mainTab_.load() == MainTab::Shelf) advanceShelfCovers();
      return;
    case MainTab::Manage:
      handleManageInput();
      return;
  }
}

void WeReadActivity::handleShelfInput() {
  const int count = static_cast<int>(std::min<uint32_t>(shelfCount_, INT32_MAX));
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout =
      shelfGridLayout(renderer, mainContentBounds(), metrics.contentSidePadding, metrics.verticalSpacing);
  const int itemsPerPage = layout.itemsPerPage;
  switch (shelfNavigationGesture_) {
    case ShelfNavigationGesture::Idle:
      if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
        shelfNavigationGesture_ = ShelfNavigationGesture::PreviousPressed;
      } else if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
        shelfNavigationGesture_ = ShelfNavigationGesture::NextPressed;
      } else {
        break;
      }
      return;
    case ShelfNavigationGesture::PreviousPressed:
    case ShelfNavigationGesture::NextPressed:
    case ShelfNavigationGesture::PreviousPageHandled:
    case ShelfNavigationGesture::NextPageHandled: {
      const bool previous = shelfNavigationGesture_ == ShelfNavigationGesture::PreviousPressed ||
                            shelfNavigationGesture_ == ShelfNavigationGesture::PreviousPageHandled;
      const bool pageHandled = shelfNavigationGesture_ == ShelfNavigationGesture::PreviousPageHandled ||
                               shelfNavigationGesture_ == ShelfNavigationGesture::NextPageHandled;
      const auto button = previous ? MappedInputManager::Button::NavPrevious : MappedInputManager::Button::NavNext;
      if (mappedInput.wasReleased(button)) {
        shelfNavigationGesture_ = ShelfNavigationGesture::Idle;
        shelfLastPageTurnAt_ = 0;
        if (pageHandled) return;

        const int selected = shelfSelected_.load();
        const int target = previous ? previousShelfIndexOrTab(selected) : ButtonNavigator::nextIndex(selected, count);
        if (target == kNoShelfSelection) {
          mainFocus_.store(MainFocus::Tabs);
          requestUpdate();
          return;
        }
        moveShelfSelection(target, itemsPerPage);
        return;
      }

      if (!mappedInput.isPressed(button)) {
        shelfNavigationGesture_ = ShelfNavigationGesture::Idle;
        shelfLastPageTurnAt_ = 0;
        return;
      }

      const uint32_t now = millis();
      if ((!pageHandled && mappedInput.getHeldTime() < kShelfPageHoldMs) ||
          (pageHandled && now - shelfLastPageTurnAt_ < kShelfPageHoldMs)) {
        return;
      }

      shelfNavigationGesture_ =
          previous ? ShelfNavigationGesture::PreviousPageHandled : ShelfNavigationGesture::NextPageHandled;
      shelfLastPageTurnAt_ = now;
      if (count > itemsPerPage) {
        const int selected = shelfSelected_.load();
        const int target = previous ? ButtonNavigator::previousPageIndex(selected, count, itemsPerPage)
                                    : ButtonNavigator::nextPageIndex(selected, count, itemsPerPage);
        moveShelfSelection(target, itemsPerPage);
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    resetShelfCoverLoading();
    activateSelected();
  }
}

void WeReadActivity::moveShelfSelection(const int index, const int itemsPerPage) {
  const int previousIndex = shelfSelected_.exchange(index);
  if (index == previousIndex) return;
  if (index / itemsPerPage != previousIndex / itemsPerPage) resetShelfCoverLoading();
  requestUpdate();
}

bool WeReadActivity::syncClockForRetry() {
  if (TimeUtils::isClockValid()) return true;

  LOG_INF("WR", "clock retry sync start: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  const bool synced = halClock.syncNow();
  const bool valid = synced && TimeUtils::isClockValid();
  LOG_INF("WR", "clock retry sync result: synced=%u valid=%u", static_cast<unsigned>(synced),
          static_cast<unsigned>(valid));
  if (!valid) {
    error_ = WeReadClient::Error::Clock;
    return false;
  }
  return true;
}

void WeReadActivity::handleErrorInput() {
  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (error_ == WeReadClient::Error::WholeBookOnly) {
    if (confirm) {
      operation_.reset();
      detailFrameValid_.store(false);
      state_.store(State::Detail);
      showCacheScopePopup();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      operation_.reset();
      detailFrameValid_.store(false);
      state_.store(State::Detail);
      requestUpdate();
    }
    return;
  }

  if (confirm) {
    if (WiFi.status() == WL_CONNECTED) {
      if (error_ == WeReadClient::Error::Clock && !syncClockForRetry()) {
        requestJobUpdate();
        return;
      }
      switch (retryJob_) {
        case Job::Sync:
          startJob(Job::Sync);
          break;
        case Job::Detail:
        case Job::Download:
          startJob(retryJob_, &pendingBook_);
          break;
      }
    } else {
      switch (retryJob_) {
        case Job::Sync:
          connectThen(Job::Sync);
          break;
        case Job::Detail:
        case Job::Download:
          connectThen(retryJob_, &pendingBook_);
          break;
      }
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    operation_.reset();
    if (retryJob_ != Job::Sync) {
      mainTab_.store(MainTab::Shelf);
      mainFocus_.store(MainFocus::Content);
    }
    state_.store(State::Home);
    requestJobUpdate();
  }
}

void WeReadActivity::handleLogoutErrorInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLogout();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state_.store(State::Home);
    requestUpdate();
  }
}

void WeReadActivity::loop() {
  if (wifiReleasePending_) {
    const bool held = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                      mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
                      mappedInput.isPressed(MappedInputManager::Button::NavNext);
    if (!held) wifiReleasePending_ = false;
    return;
  }

  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) {
    optionPopupClosing_ = !optionPopup_.isActive();
    return;
  }
  if (optionPopupClosing_) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    optionPopupClosing_ = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;
    }
  }

  const State state = state_.load();
  switch (state) {
    case State::Disclaimer:
      handleDisclaimerInput();
      return;
    case State::Home:
      handleMainInput();
      return;
    case State::Detail:
      handleDetailInput();
      return;
    case State::DetailCoverLoading:
      if (stageRenderPending_.load()) return;
      handleDetailInput();
      if (state_.load() != State::DetailCoverLoading) return;
      advanceJob();
      return;
    case State::Introduction:
      handleIntroductionInput();
      return;
    case State::Error:
      handleErrorInput();
      return;
    case State::LogoutError:
      handleLogoutErrorInput();
      return;
    case State::CacheCleared: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_.store(State::Home);
        requestUpdate();
      }
      return;
    }
    case State::CacheClearError: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        performClearCache();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_.store(State::Home);
        requestUpdate();
      }
      return;
    }
    case State::ClearingCache:
      return;
    case State::LoginConfirmed:
      requestUpdateAndWait();
      state_.store(retryJob_ == Job::Detail && detailLoaded_ ? State::DetailCoverLoading : stateForJob(retryJob_));
      return;
    case State::OpenBook:
      return;
    case State::Connecting:
    case State::Qr:
    case State::Syncing:
    case State::DetailLoading:
    case State::Downloading:
    case State::Cancelling:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    operation_.cancel();
    state_.store(State::Cancelling);
    requestJobUpdate();
    return;
  }
  if (state == State::Downloading && stageRenderPending_.load()) {
    return;
  }
  advanceJob();
}

bool WeReadActivity::isBusy(const State state) {
  return state == State::Connecting || state == State::Qr || state == State::LoginConfirmed ||
         state == State::Syncing || state == State::DetailLoading || state == State::DetailCoverLoading ||
         state == State::Downloading || state == State::Cancelling || state == State::ClearingCache;
}

const char* WeReadActivity::errorMessage() const {
  switch (error_) {
    case WeReadClient::Error::SdCard:
      return tr(STR_WEREAD_CACHE_FAILED);
    case WeReadClient::Error::Network:
      return WiFi.status() == WL_CONNECTED ? tr(STR_WEREAD_HTTP_ERROR) : tr(STR_WEREAD_NO_WIFI);
    case WeReadClient::Error::Unavailable:
      return tr(STR_WEREAD_CACHE_NOT_AVAILABLE);
    case WeReadClient::Error::WholeBookOnly:
      return tr(STR_WEREAD_CACHE_WHOLE_BOOK_ONLY);
    case WeReadClient::Error::Clock:
      return tr(STR_CLOCK_SYNC_FAIL);
    default:
      return tr(STR_WEREAD_HTTP_ERROR);
  }
}

bool WeReadActivity::drawDetailIntroduction(const Rect& bounds, const bool selected) {
  // Keep the introduction text in a stable black-on-white rendering. The
  // selection state is shown by a small marker in the gap below the preview,
  // so moving between Introduction and the action list does not require
  // redrawing every SD-card-font glyph.
  (void)selected;
  // The SD font can be loaded or reloaded after the detail page was opened
  // (for example after a font refresh). Re-resolve it here and rebuild page
  // offsets before drawing so measurement and rendering stay in sync.
  const int currentFontId = dynamicRemoteBodyFontId(renderer, UI_10_FONT_ID, "detail");
  if (currentFontId != introFontId_) buildIntroductionPages();
  // Font IDs are hashed integers and may be negative. Only zero means that
  // no font was resolved, so do not use a sign check here.
  const int bodyFontId = introFontId_ != 0 ? introFontId_ : UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(bodyFontId);
  const int textX = bounds.x;
  const int textWidth = std::max(1, bounds.width);
  const int titleY = bounds.y;
  const int textY = titleY + lineHeight + 4;
  const int maxLines = std::max(0, (bounds.y + bounds.height - textY) / lineHeight);
  constexpr bool black = true;

  renderer.drawText(UI_10_FONT_ID, textX, titleY, tr(STR_WEREAD_INTRO), black, EpdFontFamily::BOLD);

  if (maxLines == 0) return detail_.introLength > 0;
  if (!detail_.introLength) {
    renderer.drawText(UI_10_FONT_ID, textX, textY,
                      detailLoadFailed_ && !detailLoaded_ ? tr(STR_WEREAD_DETAIL_UNAVAILABLE) : tr(STR_WEREAD_NO_INTRO),
                      black);
    return false;
  }

  // buildIntroductionPages() already measured and recorded the first page's
  // line boundaries. Reuse those offsets for the detail preview instead of
  // reading and measuring the same introduction a second time.
  if (introPreviewLineCount_ > 0 && introFontId_ == bodyFontId) {
    HalFile previewFile;
    WeReadStore::BookDetailHeader previewHeader;
    if (WeReadStore::openBookDetail(WeReadStore::bookDirectory(pendingBook_.bookId), previewHeader, previewFile)) {
      const int visibleLines = std::min(maxLines, introPreviewLineCount_);
      const bool truncated = introPageCount_ > 1 || introPagesTruncated_ || visibleLines < introPreviewLineCount_;
      bool previewFailed = false;
      std::vector<std::string> previewLines;
      previewLines.reserve(visibleLines);
      std::string previewText;
      for (int lineIndex = 0; lineIndex < visibleLines; ++lineIndex) {
        const uint32_t start = introPreviewLineStarts_[lineIndex];
        const uint32_t end = introPreviewLineEnds_[lineIndex];
        const uint32_t byteCount = end >= start ? end - start : 0;
        char line[kIntroLineBufferBytes] = {};
        if (byteCount >= sizeof(line) || !previewFile.seek(WeReadStore::kBookDetailHeaderSize + start) ||
            previewFile.read(line, byteCount) != static_cast<int>(byteCount)) {
          previewFailed = true;
          break;
        }
        line[byteCount] = '\0';
        previewLines.emplace_back(line, byteCount);
        previewText.append(line, byteCount);
        previewText.push_back('\n');
      }

      if (!previewFailed) {
        // Load all visible glyph bitmaps in one batch. Drawing the lines one by
        // one without this prewarm would repeatedly hit the SD-card overflow
        // path and largely recreate the original delay.
        const uint32_t previewLength = static_cast<uint32_t>(previewText.size());
        const uint32_t previewHash = hashText(previewText);
        if (!introPreviewPrewarmFontId_ || introPreviewPrewarmFontId_ != bodyFontId ||
            introPreviewPrewarmLength_ != previewLength || introPreviewPrewarmHash_ != previewHash) {
          DynamicFont::prewarmIfSdFont(renderer, bodyFontId, previewText);
          introPreviewPrewarmFontId_ = bodyFontId;
          introPreviewPrewarmLength_ = previewLength;
          introPreviewPrewarmHash_ = previewHash;
        }
        int y = textY;
        for (int lineIndex = 0; lineIndex < visibleLines; ++lineIndex) {
          const std::string& sourceLine = previewLines[lineIndex];
          std::string displayLine = sourceLine;
          if (truncated && lineIndex + 1 == visibleLines) {
            static constexpr char kEllipsis[] = "...";
            const int ellipsisWidth = renderer.getTextAdvanceX(bodyFontId, kEllipsis, EpdFontFamily::REGULAR);
            if (introPreviewLineWidths_[lineIndex] + ellipsisWidth <= textWidth &&
                sourceLine.size() + sizeof(kEllipsis) - 1 < kIntroLineBufferBytes) {
              displayLine += kEllipsis;
            } else {
              displayLine = renderer.truncatedText(bodyFontId, sourceLine.c_str(), textWidth,
                                                    EpdFontFamily::REGULAR);
            }
          }

          if (!displayLine.empty()) {
            renderer.drawText(bodyFontId, textX, y, displayLine.c_str(), black, EpdFontFamily::REGULAR);
          }
          y += lineHeight;
        }
        return truncated;
      }
    }
  }

  HalFile file;
  WeReadStore::BookDetailHeader header;
  if (!WeReadStore::openBookDetail(WeReadStore::bookDirectory(pendingBook_.bookId), header, file) ||
      !file.seek(WeReadStore::kBookDetailHeaderSize)) {
    renderer.drawText(UI_10_FONT_ID, textX, textY, tr(STR_WEREAD_DETAIL_UNAVAILABLE), black);
    return false;
  }

  uint32_t offset = 0;
  int y = textY;
  for (int lineIndex = 0; lineIndex < maxLines && offset < header.introLength; ++lineIndex) {
    char line[192] = {};
    size_t lineLength = 0;
    int lineWidth = 0;

    while (offset < header.introLength) {
      const uint32_t glyphStart = offset;
      Utf8Glyph glyph;
      if (!readUtf8Glyph(file, header.introLength - offset, glyph)) return false;
      offset += glyph.fileBytes;
      if (glyph.textBytes == 1 && glyph.text[0] == '\r') continue;
      if (glyph.textBytes == 1 && glyph.text[0] == '\n') break;

      const int glyphWidth = renderer.getTextAdvanceX(bodyFontId, glyph.text, EpdFontFamily::REGULAR);
      if ((lineWidth > 0 && lineWidth + glyphWidth > textWidth) || lineLength + glyph.textBytes >= sizeof(line)) {
        offset = glyphStart;
        if (!file.seek(WeReadStore::kBookDetailHeaderSize + offset)) return false;
        break;
      }
      memcpy(line + lineLength, glyph.text, glyph.textBytes);
      lineLength += glyph.textBytes;
      lineWidth += glyphWidth;
    }

    const bool truncated = lineIndex + 1 == maxLines && offset < header.introLength;
    if (truncated) {
      static constexpr char kEllipsis[] = "...";
      const int ellipsisWidth = renderer.getTextAdvanceX(bodyFontId, kEllipsis, EpdFontFamily::REGULAR);
      while (lineLength > 0 &&
             (lineWidth + ellipsisWidth > textWidth || lineLength + sizeof(kEllipsis) > sizeof(line))) {
        size_t glyphStart = lineLength - 1;
        while (glyphStart > 0 && (static_cast<uint8_t>(line[glyphStart]) & 0xC0) == 0x80) --glyphStart;
        char removed[5] = {};
        const size_t removedLength = lineLength - glyphStart;
        memcpy(removed, line + glyphStart, removedLength);
        lineWidth -= renderer.getTextAdvanceX(bodyFontId, removed, EpdFontFamily::REGULAR);
        lineLength = glyphStart;
      }
      memcpy(line + lineLength, kEllipsis, sizeof(kEllipsis));
      lineLength += sizeof(kEllipsis) - 1;
    } else {
      line[lineLength] = '\0';
    }

    if (lineLength > 0) renderer.drawText(bodyFontId, textX, y, line, black, EpdFontFamily::REGULAR);
    y += lineHeight;
    if (truncated) return true;
  }
  return false;
}

void WeReadActivity::drawDisclaimer(const Rect& content) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect actions = disclaimerActionsBounds();
  const int paragraphSpacing = metrics.verticalSpacing;
  const int textWidth = std::max(0, content.width - metrics.contentSidePadding * 2);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int paragraphHeights[kDisclaimerParagraphCount] = {};
  int textHeight = paragraphSpacing * (kDisclaimerParagraphCount - 1);
  for (int i = 0; i < kDisclaimerParagraphCount; ++i) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(kDisclaimerParagraphs[i]), textWidth, 16);
    paragraphHeights[i] = static_cast<int>(lines.size()) * lineHeight;
    textHeight += paragraphHeights[i];
  }
  const int freeHeight = std::max(0, content.height - textHeight - actions.height);
  const int actionGap = std::min(freeHeight, std::max(metrics.verticalSpacing, freeHeight / 3));
  actions.y = std::min(content.y + content.height - actions.height, content.y + textHeight + actionGap);
  disclaimerActionsY_.store(actions.y);
  const Rect textBounds{
      content.x + metrics.contentSidePadding,
      content.y,
      textWidth,
      std::max(0, actions.y - actionGap - content.y),
  };
  int y = textBounds.y;
  for (int i = 0; i < kDisclaimerParagraphCount; ++i) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(kDisclaimerParagraphs[i]), textWidth, 16);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textBounds.x, y, line.c_str());
      y += lineHeight;
    }
    if (i + 1 < kDisclaimerParagraphCount) y += paragraphSpacing;
  }

  const int buttonGap = disclaimerActionGap(actions.width, metrics.verticalSpacing);
  const int buttonWidth = std::max(1, (actions.width - buttonGap) / kDisclaimerActionCount);
  const int buttonRadius = std::min(metrics.popupCornerRadius, actions.height / 2);
  const int buttonLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < kDisclaimerActionCount; ++i) {
    const int buttonX = actions.x + i * (buttonWidth + buttonGap);
    const bool selected = disclaimerSelected_ == i;
    renderer.fillRoundedRect(buttonX, actions.y, buttonWidth, actions.height, buttonRadius,
                             selected ? Color::Black : Color::White);
    renderer.drawRoundedRect(buttonX, actions.y, buttonWidth, actions.height, 1, buttonRadius, true);
    const char* label = I18N.get(kDisclaimerActions[i]);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, buttonX + (buttonWidth - labelWidth) / 2,
                      actions.y + (actions.height - buttonLineHeight) / 2, label, !selected);
  }

  if (disclaimerSaveFailed_) {
    GUI.drawPopup(renderer, tr(STR_WEREAD_DISCLAIMER_SAVE_FAILED));
  }
}

void WeReadActivity::drawShelfGrid(const Rect& content, const int selectedIndex, const int frameSelection,
                                   const bool contentFocused) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = shelfGridLayout(renderer, content, metrics.contentSidePadding, metrics.verticalSpacing);
  const int count = static_cast<int>(std::min<uint32_t>(shelfCount_, INT32_MAX));
  const int page = selectedIndex / layout.itemsPerPage;
  const int pageStart = page * layout.itemsPerPage;
  const int pageEnd = std::min(pageStart + layout.itemsPerPage, count);
  const bool incrementalFrame = frameSelection >= pageStart && frameSelection < pageEnd;

  const auto drawItem = [&](const int index, const bool selected) {
    const auto geometry = weReadShelfItemGeometry(content, layout, pageStart, pageEnd, index);
    const Rect cover = geometry.cover;
    const Rect itemBounds = geometry.hit;
    const bool focused = selected && contentFocused;
    bool foregroundBlack = true;
    if (focused) {
      foregroundBlack = GUI.drawSelectionBackground(renderer, itemBounds);
      renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
    } else if (incrementalFrame) {
      renderer.fillRect(itemBounds.x, itemBounds.y, itemBounds.width, itemBounds.height, false);
    }

    WeReadStore::ShelfRecord book;
    if (!readShelf(index, book)) return;

    const bool coverDrawn = drawCachedCover(renderer, WeReadStore::bookDirectory(book.bookId), cover);
    renderer.drawRect(cover.x, cover.y, cover.width, cover.height);
    if (!coverDrawn) {
    renderer.drawIcon(CoverIcon, cover.x + (cover.width - 32) / 2, cover.y + (cover.height - 32) / 2, 32, 32);
    }

    const int titleFontId = dynamicRemoteFontId(renderer, book.title, UI_12_FONT_ID);
    const int titleWidthLimit = layout.availableWidth;
    const std::string title =
        renderer.truncatedText(titleFontId, book.title, titleWidthLimit, EpdFontFamily::REGULAR);
    const int titleWidth = renderer.getTextAdvanceX(titleFontId, title.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(titleFontId, layout.availableX + std::max(0, (layout.availableWidth - titleWidth) / 2),
                      cover.y + cover.height + layout.titleGap, title.c_str(), foregroundBlack,
                      EpdFontFamily::REGULAR);
  };

  if (incrementalFrame) {
    drawItem(frameSelection, false);
    drawItem(selectedIndex, true);
  } else {
    for (int index = pageStart; index < pageEnd; ++index) drawItem(index, index == selectedIndex);
  }

  GUI.drawSideScrollBar(renderer, content, count, pageStart, layout.itemsPerPage);
}

void WeReadActivity::drawDetailActions(const Rect& actions, const int selectedIndex, const bool cached,
                                       const bool policyChanged) {
  GUI.drawList(
      renderer, actions, kDetailListActionCount,
      selectedIndex == static_cast<int>(DetailAction::Introduction) ? -1 : selectedIndex - 1,
      [cached, policyChanged](const int index) {
        switch (static_cast<DetailAction>(index + 1)) {
          case DetailAction::Introduction:
            return std::string();
          case DetailAction::Read:
            return std::string(I18N.get(cached ? StrId::STR_CONTINUE_READING : StrId::STR_WEREAD_ONLINE_READING));
          case DetailAction::Cache:
            if (!cached) return std::string(tr(STR_WEREAD_CACHE_BOOK));
            return std::string(
                I18N.get(policyChanged ? StrId::STR_WEREAD_UPDATE_CACHE : StrId::STR_WEREAD_RECACHE_BOOK));
          case DetailAction::Browse:
            return std::string(tr(STR_WEREAD_BROWSE_ENTRY));
          case DetailAction::Images:
            return std::string(tr(STR_WEREAD_CACHE_IMAGES));
        }
        return std::string();
      },
      nullptr, nullptr,
      [this, cached](const int index) {
        switch (static_cast<DetailAction>(index + 1)) {
          case DetailAction::Introduction:
          case DetailAction::Browse:
          case DetailAction::Cache:
            return std::string();
          case DetailAction::Read:
            return cached ? std::string() : std::string(tr(STR_WEREAD_FUTURE_SUPPORT));
          case DetailAction::Images:
            return std::string(I18N.get(detailImagePolicy_ == WeReadStore::ImagePolicy::Embed
                                            ? StrId::STR_WEREAD_OPTION_ON
                                            : StrId::STR_WEREAD_OPTION_OFF));
        }
        return std::string();
      },
      false,
      [cached](const int index) { return static_cast<DetailAction>(index + 1) == DetailAction::Read && !cached; });
}

Rect WeReadActivity::detailSelectionMarkerBounds(const Rect& content) const {
  const Rect actions = detailActionsBounds(content);
  const Rect introduction = detailIntroductionBounds(content);
  const int gapTop = introduction.y + introduction.height;
  const int gapHeight = std::max(1, actions.y - gapTop);
  const int markerHeight = std::min(4, gapHeight);
  return Rect{introduction.x, gapTop + (gapHeight - markerHeight) / 2,
              std::max(1, std::min(64, introduction.width)), markerHeight};
}

void WeReadActivity::drawDetailSelectionMarker(const Rect& content, const bool selected) {
  const Rect marker = detailSelectionMarkerBounds(content);
  renderer.fillRect(marker.x, marker.y, marker.width, marker.height, selected);
}

bool WeReadActivity::renderDetailSelectionOnly(const Rect& content) {
  if (state_.load() != State::Detail || !detailSelectionOnlyPending_.load() || !detailFrameValid_.load()) {
    return false;
  }

  const int previousSelection = detailFrameSelection_.load();
  const int selectedIndex = detailSelected_.load();
  if (previousSelection == selectedIndex) {
    detailSelectionOnlyPending_.store(false);
    return true;
  }
  const uint32_t selectionGeneration = detailSelectionGeneration_.load();

  // The framebuffer already contains the complete detail page. Repaint only
  // the action list, then submit the complete framebuffer. The X4 windowed
  // controller path can leave a rotated/ghosted region when the logical page
  // is in portrait orientation; a full-buffer commit preserves the same
  // panel refresh time while avoiding that coordinate-dependent artifact.
  const Rect actions = detailActionsBounds(content);
  renderer.fillRect(actions.x, actions.y, actions.width, actions.height, false);
  const bool cached = detailActionEnabled(DetailAction::Read);
  const bool policyChanged = cached && detailOptionsKnown_ && detailImagePolicy_ != detailSavedImagePolicy_;
  drawDetailActions(actions, selectedIndex, cached, policyChanged);
  const bool introductionChanged = previousSelection == static_cast<int>(DetailAction::Introduction) ||
                                   selectedIndex == static_cast<int>(DetailAction::Introduction);
  if (introductionChanged) {
    drawDetailSelectionMarker(content, selectedIndex == static_cast<int>(DetailAction::Introduction));
  }
  renderer.displayBuffer();
  detailFrameSelection_.store(selectedIndex);
  detailSelectionOnlyPending_.store(detailSelectionGeneration_.load() != selectionGeneration ||
                                    detailSelected_.load() != selectedIndex);
  return true;
}

void WeReadActivity::drawBookDetail(const Rect& content, const bool coverLoading, const int renderedSelection) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int side = metrics.contentSidePadding;
  const Rect cover{content.x + side, content.y, kDetailCoverWidth, kDetailCoverHeight};
  renderer.drawRect(cover.x, cover.y, cover.width, cover.height);
  const bool coverDrawn = drawCachedCover(renderer, WeReadStore::bookDirectory(pendingBook_.bookId),
                                          Rect{cover.x + 2, cover.y + 2, cover.width - 4, cover.height - 4});
  if (!coverDrawn) {
    UITheme::drawCenteredWrappedText(
        renderer, cover, UI_10_FONT_ID,
        I18N.get(coverLoading ? StrId::STR_WEREAD_COVER_LOADING : StrId::STR_WEREAD_NO_COVER), 2);
  }

  const int metaX = cover.x + cover.width + 16;
  const int metaWidth = std::max(1, content.x + content.width - metaX - side);
  const int titleFontId = dynamicRemoteFontId(renderer, detail_.title, UI_12_FONT_ID);
  const auto titleStyle = dynamicRemoteFontStyle(renderer, titleFontId);
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  const int authorFontId = dynamicRemoteFontId(renderer, detail_.author, UI_10_FONT_ID);
  const int authorLineHeight = detail_.author[0] ? renderer.getLineHeight(authorFontId) : 0;
  const int ratingLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int cacheY = cover.y + cover.height - smallLineHeight;
  const int requiredDetailHeight =
      authorLineHeight + (detail_.newRating > 0 ? ratingLineHeight : 0);
  const int maxTitleLines = std::clamp((cacheY - cover.y - requiredDetailHeight) / titleLineHeight, 1, 2);
  int metaY = cover.y;
  const auto titleLines = renderer.wrappedText(titleFontId, detail_.title, metaWidth, maxTitleLines, titleStyle);
  for (const auto& line : titleLines) {
    renderer.drawText(titleFontId, metaX, metaY, line.c_str(), true, titleStyle);
    metaY += titleLineHeight;
  }
  if (detail_.author[0]) {
    const auto author = renderer.truncatedText(authorFontId, detail_.author, metaWidth, EpdFontFamily::REGULAR);
    renderer.drawText(authorFontId, metaX, metaY, author.c_str(), true, EpdFontFamily::REGULAR);
    metaY += authorLineHeight;
  }
  if (detail_.newRating > 0) {
    char rating[48];
    snprintf(rating, sizeof(rating), tr(STR_WEREAD_RATING_FMT), detail_.newRating / 100.0);
    renderer.drawText(UI_10_FONT_ID, metaX, metaY, rating);
    metaY += ratingLineHeight;
  }

  const bool cached = Storage.exists(WeReadStore::finalBookPath(pendingBook_).c_str());
  const bool policyChanged = cached && detailOptionsKnown_ && detailImagePolicy_ != detailSavedImagePolicy_;
  const char* cacheState = !cached ? tr(STR_WEREAD_NOT_CACHED)
                                   : (policyChanged ? tr(STR_WEREAD_CACHE_NEEDS_UPDATE) : tr(STR_WEREAD_CACHE_BADGE));
  renderer.drawText(SMALL_FONT_ID, metaX, cacheY, cacheState, true, EpdFontFamily::BOLD);

  char minor[192] = {};
  if (detail_.category[0] && detail_.totalWords > 0) {
    char words[48];
    snprintf(words, sizeof(words), tr(STR_WEREAD_WORDS_FMT), static_cast<unsigned>(detail_.totalWords));
    snprintf(minor, sizeof(minor), "%s · %s", detail_.category, words);
  } else if (detail_.category[0]) {
    snprintf(minor, sizeof(minor), "%s", detail_.category);
  } else if (detail_.totalWords > 0) {
    snprintf(minor, sizeof(minor), tr(STR_WEREAD_WORDS_FMT), static_cast<unsigned>(detail_.totalWords));
  } else if (detail_.publisher[0]) {
    snprintf(minor, sizeof(minor), "%s", detail_.publisher);
  }
  const int minorFontId = dynamicRemoteFontId(renderer, minor, SMALL_FONT_ID);
  const int minorLineHeight = renderer.getLineHeight(minorFontId);
  const int minorY = cacheY - minorLineHeight;
  if (minor[0] && metaY <= minorY) {
    const auto text = renderer.truncatedText(minorFontId, minor, metaWidth, EpdFontFamily::REGULAR);
    renderer.drawText(minorFontId, metaX, minorY, text.c_str(), true, EpdFontFamily::REGULAR);
  }

  const Rect actions = detailActionsBounds(content);
  const Rect introduction = detailIntroductionBounds(content);
  const int selectedIndex = renderedSelection >= 0 ? renderedSelection : detailSelected_.load();
  detailIntroTruncated_ =
      drawDetailIntroduction(introduction, selectedIndex == static_cast<int>(DetailAction::Introduction));
  drawDetailSelectionMarker(content, selectedIndex == static_cast<int>(DetailAction::Introduction));
  drawDetailActions(actions, selectedIndex, cached, policyChanged);
}

void WeReadActivity::drawIntroduction(const Rect& content) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int side = metrics.contentSidePadding;
  const int currentFontId = dynamicRemoteBodyFontId(renderer, UI_10_FONT_ID, "page");
  if (currentFontId != introFontId_) buildIntroductionPages();
  const int bodyFontId = introFontId_ != 0 ? introFontId_ : UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(bodyFontId);
  const int maxWidth = content.width - side * 2;
  const int footerY = content.y + content.height - lineHeight;
  const uint32_t start = introPageOffsets_[introPage_];
  const uint32_t end = introPageOffsets_[introPage_ + 1];

  HalFile file;
  WeReadStore::BookDetailHeader header;
  if (!WeReadStore::openBookDetail(WeReadStore::bookDirectory(pendingBook_.bookId), header, file) ||
      !file.seek(WeReadStore::kBookDetailHeaderSize + start)) {
    renderer.drawText(UI_10_FONT_ID, content.x + side, content.y, tr(STR_WEREAD_DETAIL_UNAVAILABLE));
    return;
  }

  char line[192] = {};
  size_t lineLength = 0;
  int lineWidth = 0;
  int y = content.y;
  uint32_t offset = start;
  const auto flushLine = [&]() {
    line[lineLength] = '\0';
    if (lineLength > 0) renderer.drawText(bodyFontId, content.x + side, y, line, true, EpdFontFamily::REGULAR);
    lineLength = 0;
    lineWidth = 0;
    y += lineHeight;
  };

  while (offset < end && y < footerY) {
    Utf8Glyph glyph;
    if (!readUtf8Glyph(file, end - offset, glyph)) break;
    offset += glyph.fileBytes;
    if (glyph.textBytes == 1 && glyph.text[0] == '\r') continue;
    if (glyph.textBytes == 1 && glyph.text[0] == '\n') {
      flushLine();
      continue;
    }
    const int glyphWidth = renderer.getTextAdvanceX(bodyFontId, glyph.text, EpdFontFamily::REGULAR);
    if ((lineWidth > 0 && lineWidth + glyphWidth > maxWidth) || lineLength + glyph.textBytes >= sizeof(line)) {
      flushLine();
      if (y >= footerY) break;
    }
    memcpy(line + lineLength, glyph.text, glyph.textBytes);
    lineLength += glyph.textBytes;
    lineWidth += glyphWidth;
  }
  if (lineLength > 0 && y < footerY) flushLine();
  if (introPagesTruncated_ && introPage_ + 1 == introPageCount_ && y < footerY) {
    renderer.drawText(bodyFontId, content.x + side, y, "...", true, EpdFontFamily::REGULAR);
  }
  char page[32];
  snprintf(page, sizeof(page), tr(STR_WEREAD_PAGE_FMT), static_cast<unsigned>(introPage_ + 1),
           static_cast<unsigned>(introPageCount_));
  renderer.drawCenteredText(SMALL_FONT_ID, footerY, page);
}

void WeReadActivity::render(RenderLock&&) {
  downloadRenderPending_.store(false);
  stageRenderPending_.store(false);
  if (optionPopup_.processRender(renderer, mappedInput)) {
    // OptionPopup clears and owns the framebuffer while it is visible. The
    // underlying detail frame must be rebuilt when the popup closes.
    detailFrameValid_.store(false);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const State state = state_.load();
  const MainTab mainTab = mainTab_.load();
  const MainFocus mainFocus = mainFocus_.load();
  const int shelfSelection = shelfSelected_.load();
  const Rect content = state == State::Disclaimer ? disclaimerContentBounds()
                                                  : (state == State::Home ? mainContentBounds() : contentBounds());
  if (renderDetailSelectionOnly(content)) return;
  // ActivityManager notifications are counting notifications, so a second
  // notification can remain after a 640 ms panel refresh. If nothing changed
  // that needs painting, consume that notification without re-running the
  // expensive cover/SD-font render.
  if (state == State::Detail && state_.load() == State::Detail && detailFrameValid_.load()) return;
  detailSelectionOnlyPending_.store(false);
  // A full detail render can spend seconds loading the SD-card font. Mark the
  // cached frame invalid for its whole lifetime, so a key pressed during that
  // render cannot be mistaken for a frame that is already on the panel.
  const bool fullDetailRender = state == State::Detail;
  const int renderedDetailSelection = fullDetailRender ? detailSelected_.load() : -1;
  const uint32_t detailRenderGeneration = detailSelectionGeneration_.load();
  if (fullDetailRender) detailFrameValid_.store(false);
  const bool showingShelf = state == State::Home && mainTab == MainTab::Shelf;
  const int shelfItems = showingShelf && shelfCount_ > 0 ? shelfItemsPerPage() : 0;
  const int shelfFrameSelection = shelfFrameSelection_;
  const int shelfFrameItems = shelfFrameItemsPerPage_;
  const bool shelfFrameInvalidated = showingShelf && shelfFrameInvalidated_.exchange(false);
  const bool incrementalShelfFrame =
      showingShelf && !shelfFrameInvalidated &&
      canIncrementShelfFrame(shelfFrameSelection, shelfFrameItems, shelfSelection, shelfItems);

  if (!incrementalShelfFrame) renderer.clearScreen();
  const char* header = tr(STR_WEREAD_TITLE);
  switch (state) {
    case State::Disclaimer:
      header = tr(STR_WEREAD_DISCLAIMER_TITLE);
      break;
    case State::Downloading:
      header = tr(STR_WEREAD_TAB_SHELF);
      break;
    case State::DetailLoading:
    case State::DetailCoverLoading:
    case State::Detail:
    case State::Introduction:
      header = tr(STR_WEREAD_BOOK_DETAIL);
      break;
    case State::Home:
    case State::Connecting:
    case State::Qr:
    case State::LoginConfirmed:
    case State::Syncing:
    case State::Cancelling:
    case State::OpenBook:
    case State::Error:
    case State::LogoutError:
    case State::ClearingCache:
    case State::CacheCleared:
    case State::CacheClearError:
      break;
  }
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, header);
  if (state == State::Home) {
    mainTabs_[0].selected = mainTab == MainTab::Shelf;
    mainTabs_[1].selected = mainTab == MainTab::Manage;
    GUI.drawTabBar(renderer,
                   Rect{safe.x, safe.y + metrics.topPadding + metrics.headerHeight, safe.width, metrics.tabBarHeight},
                   mainTabs_, mainFocus == MainFocus::Tabs);
  }

  switch (state) {
    case State::Disclaimer:
      drawDisclaimer(content);
      break;
    case State::Home:
      switch (mainTab) {
        case MainTab::Shelf:
          if (shelfCount_ == 0) {
            GUI.drawPopup(renderer, tr(STR_WEREAD_SHELF_EMPTY));
          } else {
            drawShelfGrid(content, shelfSelection, incrementalShelfFrame ? shelfFrameSelection : kNoShelfSelection,
                          mainFocus == MainFocus::Content);
          }
          break;
        case MainTab::Manage:
          GUI.drawButtonMenu(
              renderer, content, kManageEntryCount, mainFocus == MainFocus::Content ? manageSelected_ : -1,
              [](const int index) { return std::string(I18N.get(kManageEntries[index].title)); }, nullptr);
          break;
      }
      break;
    case State::Detail:
      drawBookDetail(content, false, renderedDetailSelection);
      break;
    case State::DetailCoverLoading:
      drawBookDetail(content, true);
      break;
    case State::DetailLoading:
      drawBookDetail(content);
      GUI.drawPopup(renderer, tr(STR_WEREAD_FETCHING_DETAIL));
      break;
    case State::Introduction:
      drawIntroduction(content);
      break;
    case State::Qr: {
      if (!qrUrl_[0]) {
        GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
        break;
      }
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int textGap = metrics.verticalSpacing;
      const int qrLimit = content.height - textGap - lineHeight * 2;
      const int qrSide = std::max(1, std::min(content.width * 4 / 5, qrLimit));
      const int groupHeight = qrSide + textGap + lineHeight * 2;
      const int qrY = content.y + std::max(0, (content.height - groupHeight) / 2);
      QrUtils::drawQrCode(renderer, Rect{content.x + (content.width - qrSide) / 2, qrY, qrSide, qrSide}, qrUrl_);
      renderer.drawCenteredText(UI_10_FONT_ID, qrY + qrSide + textGap, tr(STR_WEREAD_SCAN_LOGIN));
      char target[64];
      snprintf(target, sizeof(target), "\"%s\"", tr(STR_WEREAD_TITLE));
      renderer.drawCenteredText(UI_10_FONT_ID, qrY + qrSide + textGap + lineHeight, target);
      break;
    }
    case State::Syncing: {
      const auto stage = progressStage_.load();
      const uint32_t completed = progressCompleted_.load();
      const uint32_t total = progressTotal_.load();
      if (stage == WeReadClient::Operation::ProgressStage::Chapters) {
        if (completed > 0) {
          char status[96];
          snprintf(status, sizeof(status), tr(STR_WEREAD_SHELF_RECEIVED), static_cast<unsigned>(completed));
          GUI.drawPopup(renderer, status);
        } else {
          GUI.drawPopup(renderer, tr(STR_WEREAD_SYNCING_SHELF));
        }
        break;
      }
      if (total == 0) {
        GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
        break;
      }
      const char* label = nullptr;
      switch (stage) {
        case WeReadClient::Operation::ProgressStage::Preparing:
          label = tr(STR_WEREAD_FETCHING_COVER_INFO);
          break;
        case WeReadClient::Operation::ProgressStage::Images:
          label = tr(STR_WEREAD_DOWNLOADING_SHELF_COVERS);
          break;
        case WeReadClient::Operation::ProgressStage::Packaging:
          label = tr(STR_WEREAD_GENERATING_COVER_THUMBNAILS);
          break;
        case WeReadClient::Operation::ProgressStage::Chapters:
          break;
      }
      char status[64];
      snprintf(status, sizeof(status), "%s %u/%u", label ? label : "", static_cast<unsigned>(completed),
               static_cast<unsigned>(total));
      drawProgressStatus(renderer, content, operation_.progressTitle(), status, completed, total);
      break;
    }
    case State::Downloading: {
      const auto stage = progressStage_.load();
      const uint32_t completed = progressCompleted_.load();
      const uint32_t total = progressTotal_.load();
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      switch (stage) {
        case WeReadClient::Operation::ProgressStage::Preparing:
        case WeReadClient::Operation::ProgressStage::Packaging: {
          const StrId* lines = kPostProcessWaitingLines;
          int lineCount = static_cast<int>(sizeof(kPostProcessWaitingLines) / sizeof(kPostProcessWaitingLines[0]));
          switch (postProcessNotice_.load()) {
            case PostProcessNotice::None:
            case PostProcessNotice::Waiting:
              break;
            case PostProcessNotice::LongWait:
              lines = kPostProcessLongWaitLines;
              lineCount = static_cast<int>(sizeof(kPostProcessLongWaitLines) / sizeof(kPostProcessLongWaitLines[0]));
              break;
          }
          const int textGap = SubpageLayout::relatedGap(metrics);
          const int titleFontId = dynamicRemoteFontId(renderer, pendingBook_.title, UI_10_FONT_ID);
          const auto titleStyle = dynamicRemoteFontStyle(renderer, titleFontId, EpdFontFamily::REGULAR);
          const int titleLineHeight = renderer.getLineHeight(titleFontId);
          const int groupHeight = titleLineHeight + lineCount * lineHeight + textGap;
          int y = content.y + std::max(0, (content.height - groupHeight) / 2);
          UITheme::drawCenteredText(renderer, content, titleFontId, y, pendingBook_.title, true, titleStyle);
          y += titleLineHeight + textGap;
          for (int i = 0; i < lineCount; ++i) {
            UITheme::drawCenteredText(renderer, content, UI_10_FONT_ID, y, I18N.get(lines[i]));
            y += lineHeight;
          }
          break;
        }
        case WeReadClient::Operation::ProgressStage::Chapters:
        case WeReadClient::Operation::ProgressStage::Images: {
          const char* label = stage == WeReadClient::Operation::ProgressStage::Chapters
                                  ? tr(STR_WEREAD_CACHING_CHAPTERS)
                                  : tr(STR_WEREAD_DOWNLOADING_IMAGES);
          char status[64];
          if (total == 0) {
            snprintf(status, sizeof(status), "%s", label);
          } else {
            snprintf(status, sizeof(status), "%s %u/%u", label, static_cast<unsigned>(completed),
                     static_cast<unsigned>(total));
          }
          drawProgressStatus(renderer, content, pendingBook_.title, status, completed, total);
          break;
        }
      }
      break;
    }
    case State::Error:
      GUI.drawPopup(renderer, errorMessage());
      break;
    case State::LogoutError:
      GUI.drawPopup(renderer, tr(STR_WEREAD_LOGOUT_FAILED));
      break;
    case State::ClearingCache:
      GUI.drawPopup(renderer, tr(STR_CLEARING_CACHE));
      break;
    case State::CacheCleared:
      GUI.drawPopup(renderer, tr(STR_CACHE_CLEARED));
      break;
    case State::CacheClearError:
      GUI.drawPopup(renderer, tr(STR_CLEAR_CACHE_FAILED));
      break;
    case State::LoginConfirmed:
      GUI.drawPopup(renderer, tr(STR_WEREAD_LOGIN_CONFIRMED));
      break;
    case State::Connecting:
      if (retryJob_ == Job::Sync) {
        GUI.drawPopup(renderer, tr(STR_WEREAD_SYNCING_SHELF));
      } else {
        GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
      }
      break;
    case State::Cancelling:
    case State::OpenBook:
      GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
      break;
  }

  const char* back = "";
  const char* confirm = "";
  const char* previous = "";
  const char* next = "";
  switch (state) {
    case State::Disclaimer:
      back = tr(STR_CANCEL);
      confirm = tr(STR_SELECT);
      previous = tr(STR_DIR_LEFT);
      next = tr(STR_DIR_RIGHT);
      break;
    case State::Home:
      back = tr(STR_BACK);
      switch (mainFocus) {
        case MainFocus::Tabs:
          confirm = tr(STR_SELECT);
          previous = tr(STR_DIR_LEFT);
          next = tr(STR_DIR_RIGHT);
          break;
        case MainFocus::Content:
          switch (mainTab) {
            case MainTab::Shelf:
              confirm = tr(STR_OPEN);
              previous = tr(STR_DIR_LEFT);
              next = tr(STR_DIR_RIGHT);
              break;
            case MainTab::Manage:
              confirm = tr(STR_SELECT);
              previous = tr(STR_DIR_UP);
              next = tr(STR_DIR_DOWN);
              break;
          }
          break;
      }
      break;
    case State::Detail:
    case State::DetailCoverLoading:
      back = tr(STR_BACK);
      confirm = tr(STR_SELECT);
      previous = tr(STR_DIR_UP);
      next = tr(STR_DIR_DOWN);
      break;
    case State::Introduction:
      back = tr(STR_BACK);
      previous = tr(STR_DIR_UP);
      next = tr(STR_DIR_DOWN);
      break;
    case State::Error:
    case State::LogoutError:
      back = tr(STR_BACK);
      confirm = state == State::Error && error_ == WeReadClient::Error::WholeBookOnly ? tr(STR_SELECT) : tr(STR_RETRY);
      break;
    case State::CacheCleared:
      back = tr(STR_BACK);
      break;
    case State::CacheClearError:
      back = tr(STR_BACK);
      confirm = tr(STR_RETRY);
      break;
    case State::Connecting:
    case State::Qr:
    case State::Syncing:
    case State::DetailLoading:
    case State::Downloading:
    case State::Cancelling:
      back = tr(STR_CANCEL);
      break;
    case State::LoginConfirmed:
    case State::OpenBook:
    case State::ClearingCache:
      break;
  }
  const auto labels = mappedInput.mapLabels(back, confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (showingShelf) {
    if (shelfFrameInvalidated_.load()) {
      shelfFrameSelection_ = kNoShelfSelection;
      shelfFrameItemsPerPage_ = 0;
      requestUpdate(true);
      return;
    }
    if (shelfCount_ > 0) {
      // The framebuffer contains this snapshot even when the stale panel update below is skipped.
      shelfFrameSelection_ = shelfSelection;
      shelfFrameItemsPerPage_ = shelfItems;
      if (shelfSelected_.load() != shelfSelection) {
        requestUpdate(true);
        return;
      }
    } else {
      shelfFrameSelection_ = kNoShelfSelection;
      shelfFrameItemsPerPage_ = 0;
    }
  } else {
    shelfFrameSelection_ = kNoShelfSelection;
    shelfFrameItemsPerPage_ = 0;
  }
  if (state == State::Home &&
      (state_.load() != State::Home || mainTab_.load() != mainTab || mainFocus_.load() != mainFocus)) {
    requestUpdate(true);
    return;
  }
  renderer.displayBuffer();
  if (state == State::Detail && state_.load() == State::Detail) {
    // Commit the selection that was actually drawn, after the blocking panel
    // transfer. Input may have changed detailSelected_ while the transfer was
    // in progress; keep that newer selection pending for the next partial
    // frame instead of declaring it already visible.
    detailFrameSelection_.store(renderedDetailSelection);
    detailFrameValid_.store(true);
    detailSelectionOnlyPending_.store(detailSelectionGeneration_.load() != detailRenderGeneration ||
                                      detailSelected_.load() != renderedDetailSelection);
  } else {
    detailFrameValid_.store(false);
  }
}

bool WeReadActivity::preventAutoSleep() {
  const State state = state_.load();
  return isBusy(state) || (state == State::Home && operation_.active());
}
