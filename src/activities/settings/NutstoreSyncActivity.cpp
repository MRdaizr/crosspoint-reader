#include "NutstoreSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <memory>

#include "MappedInputManager.h"
#include "NutstoreConfigStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void NutstoreSyncActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  status.phase = NutstoreSyncPhase::IDLE;
  status.message = "Select Wi-Fi for Nutstore sync";
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void NutstoreSyncActivity::onWifiSelectionComplete(bool connected) {
  if (!connected) {
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = "Wi-Fi connection failed";
    finished = true;
    success = false;
    requestUpdate();
    return;
  }
  status.phase = NutstoreSyncPhase::ENUMERATING;
  status.message = "Starting Nutstore sync...";
  requestUpdateAndWait();
  runSync();
}

void NutstoreSyncActivity::runSync() {
  NUTSTORE_CONFIG.loadFromFile();
  success = NutstoreSync::run(
      NUTSTORE_CONFIG.get(), status,
      [this](const NutstoreSyncStatus& s) {
        status = s;
        requestUpdate(true);
      },
      &cancelRequested);
  finished = true;
  requestUpdate();
}

void NutstoreSyncActivity::loop() {
  if (finished) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) &&
      status.phase != NutstoreSyncPhase::DOWNLOADING && status.phase != NutstoreSyncPhase::DELETING) {
    cancelRequested = true;
  }
}

void NutstoreSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Nutstore Sync");

  if (status.phase == NutstoreSyncPhase::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Nutstore sync complete", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(
        UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
        ("Downloaded " + std::to_string(status.downloaded) + ", skipped " + std::to_string(status.skipped) +
         ", deleted " + std::to_string(status.deleted))
            .c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (status.phase == NutstoreSyncPhase::FAILED || status.phase == NutstoreSyncPhase::CANCELLED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              status.phase == NutstoreSyncPhase::FAILED ? tr(STR_UPDATE_FAILED) : "Cancelled", true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, status.message.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, status.message.c_str(), true, EpdFontFamily::BOLD);
    int y = top + lineHeight + metrics.verticalSpacing;
    const int pct = status.total > 0 ? static_cast<int>((status.processed * 100) / status.total) : 0;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        pct, 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing + lineHeight + metrics.verticalSpacing;
    if (!status.currentFile.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, status.currentFile.c_str());
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y, NutstoreSync::phaseName(status.phase));
    }
  }

  renderer.displayBuffer();
}
