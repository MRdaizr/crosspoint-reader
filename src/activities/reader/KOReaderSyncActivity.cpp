#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <utility>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                  : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

}  // namespace

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath, const int currentSpineIndex,
                                           const int currentPage, const int totalPagesInSpine,
                                           SavedProgressPosition localKoPos, std::string localChapterName,
                                           std::optional<uint16_t> currentParagraphIndex)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      currentSpineIndex(currentSpineIndex),
      currentPage(currentPage),
      totalPagesInSpine(totalPagesInSpine),
      currentParagraphIndex(currentParagraphIndex),
      localChapterName(std::move(localChapterName)),
      remoteProgress{},
      remotePosition{},
      localProgress(std::move(localKoPos)) {}

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(const int spineIndex, const int page,
                                                 std::optional<uint32_t> visibleTextOffset) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  if (!visibleTextOffset.has_value() && remotePosition.hasVisibleTextOffset && remotePosition.spineIndex == spineIndex) {
    visibleTextOffset = remotePosition.visibleTextOffset;
  }
  LOG_DBG("KOSync", "Applying remote position: spine=%d page=%d offset=%s", spineIndex, page,
          visibleTextOffset.has_value() ? std::to_string(*visibleTextOffset).c_str() : "none");
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, visibleTextOffset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");
  WiFi.setSleep(false);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  const std::string primaryHash = documentHash;
  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress. Smart mode probes both document-id methods so a
  // different KOReader device can still be found when it used the other
  // matching method.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        result = KOReaderSyncClient::OK;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  if (!remotePosition.hasVisibleTextOffset && remoteProgress.position.has_value()) {
    // The standard XPath is attempted first.  If it cannot be resolved (for
    // example after a publisher rewrites the XHTML), use the CrossPoint rich
    // position as a bounded page/paragraph fallback.
    const bool sameXPath = remoteProgress.position->xpath == remoteProgress.progress;
    if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer, sameXPath)) {
      remotePosition = *richMapped;
      LOG_DBG("KOSync", "Applied rich remote position: spine=%d page=%d/%d", remotePosition.spineIndex,
              remotePosition.pageNumber, remotePosition.totalPages);
    }
  }

  if (smartSyncEnabled()) {
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: doc=%s local=%.6f remote=%.6f delta=%.6f remoteXpath=%s mapped=%d/%d",
            documentHash.c_str(), localProgress.percentage, remoteProgress.percentage, delta,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);

    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }

    if (delta > 0) {
      // Alternate hashes are probes for remote state only. Upload on the
      // configured primary method so the primary record is healed.
      documentHash = primaryHash;
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
    return;
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Send the page/paragraph hints only to the CrossPoint endpoint.  The
  // standard KOReader service rejects unknown position fields, while the
  // CrossPoint server can use them for lossless same-device re-pagination.
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition position;
    const float percentage = std::clamp(localProgress.percentage, 0.0f, 1.0f);
    position.pctQ = static_cast<uint32_t>(percentage * 1000000.0f + 0.5f);
    position.spineIndex = static_cast<uint16_t>(std::max(0, currentSpineIndex));
    position.pageNumber = static_cast<uint16_t>(std::max(0, currentPage));
    position.totalPages = static_cast<uint16_t>(std::max(1, totalPagesInSpine));
    position.paragraphIndex = currentParagraphIndex;
    position.xpath = localProgress.xpath;
    progress.position = std::move(position);
  }

  if (KOREADER_STORE.getSendMetadata()) {
    ensureEpubLoaded();
    KOReaderMetadata metadata;
    const size_t slash = epubPath.rfind('/');
    metadata.filename = slash == std::string::npos ? epubPath : epubPath.substr(slash + 1);
    if (epub) {
      metadata.title = epub->getTitle();
      metadata.authors = epub->getAuthor();
    }
    progress.metadata = std::move(metadata);
  }

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  app.on(ACTION_ROW, &KOReaderSyncActivity::onResultRow, this);
  app.setScreen(&KOReaderSyncActivity::resultScreen, this);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();
  closeRouting();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::chooseResultOption() {
  if (selectedOption == 0) {
    const std::optional<uint32_t> visibleOffset = remotePosition.hasVisibleTextOffset
                                                       ? std::optional<uint32_t>(remotePosition.visibleTextOffset)
                                                       : std::nullopt;
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber, visibleOffset);
  } else {
    performUpload();
  }
}

void KOReaderSyncActivity::startUpload() {
  if (documentHash.empty()) {
    documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                       ? KOReaderDocumentId::calculateFromFilename(epubPath)
                       : KOReaderDocumentId::calculate(epubPath);
  }
  performUpload();
}

void KOReaderSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KOReaderSyncActivity*>(user);
  if (event.value < 0 || (self->state == SHOWING_RESULT && event.value > 1) ||
      (self->state != SHOWING_RESULT && self->state != NO_REMOTE_PROGRESS)) {
    return;
  }
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    self->selectedOption = event.value;
    self->chooseResultOption();
  } else {
    self->startUpload();
  }
}

void KOReaderSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<KOReaderSyncActivity*>(user)->buildResultScreen(screen);
}

void KOReaderSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == SHOWING_RESULT) {
    if (!epub) return;
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        remoteTocIndex >= 0 ? epub->getTocItem(remoteTocIndex).title
                            : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char remoteVal[64];
    snprintf(remoteVal, sizeof(remoteVal), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    char localVal[64];
    snprintf(localVal, sizeof(localVal), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    char deviceStr[80];
    deviceStr[0] = '\0';
    if (!remoteProgress.device.empty()) {
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
    }

    auto labelStyle = screen.theme().bodyText;
    labelStyle.bold = true;
    auto detailStyle = screen.theme().smallText;
    const int16_t labelH = screen.target().lineHeight(labelStyle.font);
    const int16_t detailH = screen.target().lineHeight(detailStyle.font);
    const int16_t indent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
    const auto addLine = [&](const char* text, const fui::TextStyle& style, int16_t height, int16_t gap) {
      auto rect = screen.takeTop(height, gap);
      rect.x = static_cast<int16_t>(rect.x + indent);
      rect.width = static_cast<int16_t>(rect.width - indent);
      screen.target().text(rect, text, style);
    };
    addLine(tr(STR_REMOTE_LABEL), labelStyle, labelH, screen.theme().spaceSm);
    addLine(remoteChapter.c_str(), detailStyle, detailH, screen.theme().spaceXs);
    addLine(remoteVal, detailStyle, detailH, screen.theme().spaceXs);
    if (deviceStr[0] != '\0') addLine(deviceStr, detailStyle, detailH, screen.theme().spaceXs);
    screen.spacer(screen.theme().spaceLg);
    addLine(tr(STR_LOCAL_LABEL), labelStyle, labelH, screen.theme().spaceSm);
    addLine(localChapter.c_str(), detailStyle, detailH, screen.theme().spaceXs);
    addLine(localVal, detailStyle, detailH, screen.theme().spaceXs);

    screen.spacer(screen.theme().spaceMd);
    fui::ListItem actions[2]{};
    actions[0].label = tr(STR_APPLY_REMOTE);
    actions[0].actionValue = 0;
    actions[0].icon = {};
    actions[1].label = tr(STR_UPLOAD_LOCAL);
    actions[1].actionValue = 1;
    actions[1].icon = {};
    fui::ListProps props;
    props.items = actions;
    props.count = 2;
    props.selectedIndex = static_cast<int16_t>(selectedOption);
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.scrollIndicator = false;
    if (!mappedInput.hasTouch()) props.rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    const int16_t rowHeight = props.rowHeight > 0 ? props.rowHeight : screen.theme().rowHeight;
    screen.list(props, static_cast<int16_t>(rowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm));
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);
    fui::ListItem action{};
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    action.icon = {};
    fui::ListProps props;
    props.items = &action;
    props.count = 1;
    props.selectedIndex = 0;
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.scrollIndicator = false;
    if (!mappedInput.hasTouch()) props.rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    const int16_t rowHeight = props.rowHeight > 0 ? props.rowHeight : screen.theme().rowHeight;
    screen.list(props, static_cast<int16_t>(rowHeight + screen.theme().spaceMd), fui::LayoutAnchor::Bottom);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  if (state == SHOWING_RESULT || state == NO_REMOTE_PROGRESS) {
    // The comparison and upload choices are FUI rows; the surrounding header
    // and status screens keep the existing reader chrome.
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                              state == SHOWING_RESULT ? tr(STR_SELECT) : tr(STR_UPLOAD),
                                              state == SHOWING_RESULT ? tr(STR_DIR_UP) : "",
                                              state == SHOWING_RESULT ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    const auto route = routeTouch(mappedInput);
    if (route) return;
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Keep physical-button activation on the same path as FUI row taps so
      // visible-text offsets from a rich remote position are preserved.
      chooseResultOption();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    const auto route = routeTouch(mappedInput);
    if (route) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
