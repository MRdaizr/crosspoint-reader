#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <cctype>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::activateSelected() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[static_cast<size_t>(selectorIndex)];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::onRowEvent(const freeink::ui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING || event.value < 0 ||
      event.value >= static_cast<int>(self->entries.size())) return;
  self->selectorIndex = event.value;
  self->listNav.selected = event.value;
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state == BrowserState::BROWSING) self->buildBrowsingScreen(screen);
  else self->buildStatusScreen(screen);
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  closeRouting();
  entries.clear();
  rowItems.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    const auto route = routeTouch(mappedInput);
    if (route) return;

    if (!entries.empty()) {
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(entries.size()))) requestUpdate();
        return;
      }
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        listNav.selected = selectorIndex;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        listNav.selected = selectorIndex;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), listNav.pageRows());
        listNav.selected = selectorIndex;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), listNav.pageRows());
        listNav.selected = selectorIndex;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      });
    }
  }
}

void OpdsBookBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  for (const auto& entry : entries) {
    fui::ListItem item;
    item.label = entry.title.c_str();
    item.subtitle = (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) ? entry.author.c_str() : nullptr;
    item.value = entry.type == OpdsEntryType::NAVIGATION ? ">" : nullptr;
    item.actionValue = static_cast<int16_t>(rowItems.size());
    item.icon = {};
    rowItems.push_back(item);
  }
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = rowItems.data(); props.count = static_cast<uint16_t>(rowItems.size()); props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch; props.valueInset = 8; props.subtitleText = screen.theme().smallText; props.subtitleText.maxLines = 1;
  const int16_t rowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listWithSubtitleRowHeight);
  props.rowHeight = rowHeight;
  listNav.selected = selectorIndex;
  listNav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, static_cast<int>(rowItems.size()), props);
  screen.list(props);
}

void OpdsBookBrowserActivity::buildStatusScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.centeredText(state == BrowserState::ERROR ? errorMessage.c_str() : statusMessage.c_str(), screen.theme().bodyText);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);

  if (state == BrowserState::BROWSING || state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING ||
      state == BrowserState::ERROR) {
    renderUi();
  } else if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 40, tr(STR_DOWNLOADING));
    const auto text = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), renderer.getScreenWidth() - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 10, text.c_str());
    if (downloadTotal > 0) GUI.drawProgressBar(renderer, Rect{50, renderer.getScreenHeight() / 2 + 20,
                                                               renderer.getScreenWidth() - 100, 20}, downloadProgress, downloadTotal);
  }

  const bool hasSelectedEntry = selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size());
  const char* confirmLabel = state == BrowserState::BROWSING && hasSelectedEntry &&
                                     entries[static_cast<size_t>(selectorIndex)].type == OpdsEntryType::BOOK
                                 ? tr(STR_DOWNLOAD)
                                 : (state == BrowserState::BROWSING ? tr(STR_OPEN) : "");
  const char* searchLabel = state == BrowserState::BROWSING && !searchTemplate.empty() && selectorIndex == 0
                                ? tr(STR_SEARCH)
                                : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(state == BrowserState::DOWNLOADING ? tr(STR_CANCEL) : tr(STR_BACK),
                                            confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;
  listNav.reset();
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  rebuildRowItems();
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  closeRouting();
  entries.clear();
  rowItems.clear();
  selectorIndex = 0;
  listNav.reset();
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    closeRouting();
    entries.clear();
    rowItems.clear();
    selectorIndex = 0;
    listNav.reset();
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  const char* folder = SETTINGS.opdsDownloadFolder;
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title,
                                static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  closeRouting();
  entries.clear();
  rowItems.clear();
  selectorIndex = 0;
  listNav.reset();
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
