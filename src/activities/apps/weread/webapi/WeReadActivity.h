#pragma once

#include <atomic>
#include <cstdint>

#include "../WeReadBackend.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UiAppHost.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

struct Rect;

class WeReadActivity final : public Activity, private UiAppHost {
 public:
  WeReadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WeRead", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    Disclaimer,
    Home,
    Connecting,
    Qr,
    LoginConfirmed,
    Syncing,
    DetailLoading,
    DetailCoverLoading,
    Detail,
    Introduction,
    Downloading,
    Cancelling,
    OpenBook,
    Error,
    LogoutError,
    ClearingCache,
    CacheCleared,
    CacheClearError
  };
  enum class MainTab : uint8_t { Shelf, Manage };
  enum class MainFocus : uint8_t { Tabs, Content };
  enum class Job : uint8_t { Sync, Detail, Download };
  enum class DetailAction : uint8_t { Introduction, Read, Browse, Cache, Images };
  enum class PostProcessNotice : uint8_t { None, Waiting, LongWait };
  enum class ShelfNavigationGesture : uint8_t {
    Idle,
    PreviousPressed,
    NextPressed,
    PreviousPageHandled,
    NextPageHandled
  };
  static constexpr int kDetailActionCount = 5;
  static constexpr int kDetailListActionCount = kDetailActionCount - 1;
  static constexpr int kMaxIntroPages = 128;
  static constexpr int kMaxIntroPreviewLines = 64;
  static constexpr uint32_t kLongWaitMs = 30000;

  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  WeReadClient::Operation operation_;
  mutable HalFile shelfFile_;
  std::atomic<State> state_{State::Disclaimer};
  std::atomic<WeReadClient::Operation::ProgressStage> progressStage_{WeReadClient::Operation::ProgressStage::Chapters};
  std::atomic<uint32_t> progressCompleted_{0};
  std::atomic<uint32_t> progressTotal_{0};
  std::atomic<PostProcessNotice> postProcessNotice_{PostProcessNotice::None};
  std::atomic<int> disclaimerActionsY_{-1};
  WeReadClient::Error error_ = WeReadClient::Error::Ok;
  WeReadStore::ShelfRecord pendingBook_;
  char qrUrl_[256] = {};
  uint32_t shelfCount_ = 0;
  int disclaimerSelected_ = 0;
  int manageSelected_ = 0;
  std::atomic<int> shelfSelected_{0};
  int shelfFrameSelection_ = -1;
  int shelfFrameItemsPerPage_ = 0;
  std::atomic<bool> shelfFrameInvalidated_{true};
  int shelfCoverPageStart_ = -1;
  int shelfCoverCursor_ = 0;
  ShelfNavigationGesture shelfNavigationGesture_ = ShelfNavigationGesture::Idle;
  uint32_t shelfLastPageTurnAt_ = 0;
  std::atomic<int> detailSelected_{0};
  std::atomic<int> detailFrameSelection_{-1};
  std::atomic<bool> detailFrameValid_{false};
  std::atomic<bool> detailSelectionOnlyPending_{false};
  std::atomic<uint32_t> detailSelectionGeneration_{0};
  int introPage_ = 0;
  int introPageCount_ = 1;
  uint32_t postProcessStartedAt_ = 0;
  std::atomic<MainTab> mainTab_{MainTab::Shelf};
  std::atomic<MainFocus> mainFocus_{MainFocus::Content};
  Job retryJob_ = Job::Sync;
  WeReadClient::Operation::ShelfCoverScope syncShelfCoverScope_ = WeReadClient::Operation::ShelfCoverScope::None;
  WeReadStore::BookDetailHeader detail_;
  uint32_t introPageOffsets_[kMaxIntroPages + 1] = {};
  uint32_t introPreviewLineStarts_[kMaxIntroPreviewLines] = {};
  uint32_t introPreviewLineEnds_[kMaxIntroPreviewLines] = {};
  uint16_t introPreviewLineWidths_[kMaxIntroPreviewLines] = {};
  int introPreviewLineCount_ = 0;
  int introFontId_ = 0;
  uint32_t introPreviewPrewarmHash_ = 0;
  uint32_t introPreviewPrewarmLength_ = 0;
  int introPreviewPrewarmFontId_ = 0;
  WeReadStore::ImagePolicy detailImagePolicy_ = WeReadStore::ImagePolicy::Embed;
  WeReadStore::ImagePolicy detailSavedImagePolicy_ = WeReadStore::ImagePolicy::Embed;
  WeReadClient::DownloadOptions::ChapterScope downloadChapterScope_ =
      WeReadClient::DownloadOptions::ChapterScope::WholeBook;
  bool detailLoaded_ = false;
  bool detailLoadFailed_ = false;
  bool detailOptionsKnown_ = false;
  bool detailIntroTruncated_ = false;
  bool introPagesTruncated_ = false;
  bool shelfCoverStopped_ = false;
  bool optionPopupClosing_ = false;
  bool disclaimerSaveFailed_ = false;
  bool wifiSessionActive_ = false;
  bool wifiReleasePending_ = false;
  std::atomic<bool> downloadRenderPending_{false};
  std::atomic<bool> stageRenderPending_{false};

  // FUI owns the tab strip, manage list, and detail action list. Shelf art,
  // streamed text, QR and progress surfaces remain renderer-owned.
  static constexpr freeink::ui::ActionId ACTION_FUI_TAB = 1;
  static constexpr freeink::ui::ActionId ACTION_FUI_MANAGE = 2;
  static constexpr freeink::ui::ActionId ACTION_FUI_DETAIL = 3;
  static constexpr int kFuiManageCount = 3;
  static constexpr int kFuiTabCount = 2;
  static void fuiScreen(UiScreen& screen, void* user);
  static void onFuiAction(const freeink::ui::ActionEvent& event, void* user);
  void buildFuiScreen(UiScreen& screen);
  bool routeFuiTouch();
  freeink::ui::TabItem fuiMainTabs_[kFuiTabCount]{};
  freeink::ui::TabBarProps fuiTabProps_{};
  freeink::ui::ListItem fuiManageItems_[kFuiManageCount]{};
  freeink::ui::ListProps fuiManageProps_{};
  freeink::ui::ListNav fuiManageNav_;
  freeink::ui::ListItem fuiDetailItems_[kDetailListActionCount]{};
  freeink::ui::ListProps fuiDetailProps_{};
  freeink::ui::ListNav fuiDetailNav_;

  bool refreshShelf();
  bool readShelf(int index, WeReadStore::ShelfRecord& record) const;
  Rect contentBounds() const;
  Rect mainContentBounds() const;
  Rect detailActionsBounds(const Rect& content) const;
  Rect detailIntroductionBounds(const Rect& content) const;
  Rect disclaimerSafeBounds() const;
  Rect disclaimerContentBounds() const;
  Rect disclaimerActionsBounds() const;
  int shelfItemsPerPage() const;
  void resetShelfCoverLoading();
  void advanceShelfCovers();
  WeReadClient::Operation::Event stepOperation();
  void updatePostProcessNotice(WeReadClient::Operation::ProgressStage previous,
                               WeReadClient::Operation::ProgressStage current);
  void maybeShowLongWait(RenderLock& renderBarrier);
  void requestDownloadUpdate();
  void requestJobUpdate();
  void startJob(Job job, const WeReadStore::ShelfRecord* book = nullptr);
  void connectThen(Job job, const WeReadStore::ShelfRecord* book = nullptr);
  void activateSelected();
  void openSelectedDetail(const WeReadStore::ShelfRecord& book);
  void loadSelectedDetail(bool preserveUi = false);
  static DetailAction detailActionForRow(int row);
  static int detailActionRow(DetailAction action);
  bool detailActionEnabled(DetailAction action) const;
  void moveDetailSelection(int direction);
  void activateDetailSelection();
  void showCacheScopePopup();
  void showShelfRefreshPopup();
  void startBookDownload();
  void selectChapterRange();
  void cancelChapterRangeSelection();
  void failChapterRangeSelection(WeReadClient::Error error);
  void handleDetailInput();
  void handleIntroductionInput();
  void buildIntroductionPages();
  bool drawDetailIntroduction(const Rect& bounds, bool selected);
  void drawDetailActions(const Rect& actions, int selectedIndex, bool cached, bool policyChanged);
  Rect detailSelectionMarkerBounds(const Rect& content) const;
  void drawDetailSelectionMarker(const Rect& content, bool selected);
  bool renderDetailSelectionOnly(const Rect& content);
  void drawShelfGrid(const Rect& content, int selectedIndex, int frameSelection, bool contentFocused);
  void drawDisclaimer(const Rect& content);
  void drawBookDetail(const Rect& content, bool coverLoading = false, int selectedIndex = -1);
  void drawIntroduction(const Rect& content);
  void updateJobProgress();
  void advanceJob();
  void openBook(const char* path);
  void openShelf();
  void syncShelf(WeReadClient::Operation::ShelfCoverScope scope = WeReadClient::Operation::ShelfCoverScope::None);
  void enterApp();
  void activateDisclaimerSelection();
  void promptClearCache();
  void performClearCache();
  void promptLogout();
  void performLogout();
  void handleDisclaimerInput();
  void handleMainInput();
  void handleMainTabInput();
  void handleManageInput();
  void handleShelfInput();
  void selectMainTab(MainTab tab);
  void moveShelfSelection(int index, int itemsPerPage);
  bool syncClockForRetry();
  void handleErrorInput();
  void handleLogoutErrorInput();
  const char* errorMessage() const;
  static State stateForJob(Job job);
  static bool isBusy(State state);
};
