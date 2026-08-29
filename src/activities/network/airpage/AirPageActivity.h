#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../../Activity.h"
#include "AirPageConnection.h"
#include "AirPageImageStore.h"
#include "components/UiAppHost.h"

struct Rect;

class AirPageActivity final : public Activity, private UiAppHost {
 public:
  explicit AirPageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AirPage", renderer, mappedInput), UiAppHost(renderer), connection_(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class Screen : uint8_t { Qr, Settings, History, Image };
  enum class Phase : uint8_t { Idle, FetchRequested, Fetching, WallpaperWriting, WallpaperNotice };
  enum class Notice : uint8_t {
    None,
    NoImage,
    InvalidImage,
    WifiRequired,
    WifiFailed,
    DownloadFailed,
    RealtimeRetrying,
    RealtimePaused,
    SettingsSaveFailed,
    WallpaperFailed,
  };
  enum class SettingRow : uint8_t { Mode, AutoWallpaper, Count };
  enum class WallpaperResult : uint8_t { None, Saved, Failed };
  enum class ImageDisplayResult : uint8_t { None, Success, Failure };
  bool processImageDisplayResult();
  void applyConnectionEvent(airpage::AirPageConnection::Event event);
  void clearConnectionNotice();

  void openSettings();
  void applySettingsSelection();
  void openHistory();
  void openSelectedHistoryImage();
  void openWallpaperConfirmation();
  void handleWallpaperResult(const ActivityResult& result);
  void handleRefresh();
  void queueFetch();
  void doFetch();
  void openWifiSelection(bool fetchAfterConnect);
  void handleWifiResult(const ActivityResult& result, bool fetchAfterConnect);
  bool consumeInputReleaseBarrier();
  void setAirPageScreen(Screen screen);

  // FUI owns the settings/history list interaction table; QR/image/status
  // pages continue using their specialised renderer paths.
  static void fuiScreen(UiScreen& screen, void* user);
  static void onFuiRow(const freeink::ui::ActionEvent& event, void* user);
  void buildFuiScreen(UiScreen& screen);
  bool routeFuiTouch();
  std::vector<std::string> fuiRowLabels_;
  std::vector<std::string> fuiRowValues_;
  std::vector<freeink::ui::ListItem> fuiRows_;
  freeink::ui::ListNav fuiNav_;

  void renderQr(const Rect& viewport);
  void renderStatus(const Rect& viewport, const char* msg);
  Rect contentViewport() const;
  const char* noticeText() const;
  const char* connectionText() const;
  const char* refreshActionText() const;
  const char* screenTitle() const;

  static constexpr int kSettingsRows = static_cast<int>(SettingRow::Count);

  Screen screen_ = Screen::Qr;
  Phase phase_ = Phase::Idle;
  Notice notice_ = Notice::None;
  WallpaperResult wallpaperResult_ = WallpaperResult::None;
  std::atomic<ImageDisplayResult> imageDisplayResult_{ImageDisplayResult::None};

  airpage::AirPageImageStore imageStore_;
  airpage::AirPageConnection connection_;
  airpage::SelectedImage selectedImage_;
  bool imageNeedsDisplay_ = true;
  bool waitForInputRelease_ = false;
  bool autoSleepWallpaper_ = false;
  int displayedScreenWidth_ = 0;
  int displayedScreenHeight_ = 0;
  int settingsSelection_ = 0;
  int historySelection_ = 0;

  // These short strings are required by the HTTP and QR APIs. They are built
  // once per activity lifetime and reused.
  std::string uploadUrl_;
  std::string downloadUrl_;
  std::string legacyDownloadUrl_;
};
