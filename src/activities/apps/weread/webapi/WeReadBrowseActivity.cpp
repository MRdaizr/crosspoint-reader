#include "WeReadBrowseActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

namespace {

struct Utf8Glyph {
  char text[5] = {};
  uint8_t fileBytes = 0;
  uint8_t textBytes = 0;
};

bool readUtf8Glyph(HalFile& file, const uint32_t remaining, Utf8Glyph& glyph) {
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
      glyph.textBytes = 1;
      return true;
    }
    const int next = file.read();
    if (next < 0) return false;
    ++glyph.fileBytes;
    if ((next & 0xC0) != 0x80) {
      glyph.text[0] = '?';
      glyph.textBytes = 1;
      return true;
    }
    glyph.text[glyph.textBytes++] = static_cast<char>(next);
  }
  glyph.text[glyph.textBytes] = '\0';
  return true;
}

constexpr StrId kMenuTitles[] = {
    StrId::STR_WEREAD_BROWSE_POPULAR_HIGHLIGHTS,
    StrId::STR_WEREAD_BROWSE_MY_HIGHLIGHTS,
    StrId::STR_WEREAD_BROWSE_POPULAR_REVIEWS,
    StrId::STR_WEREAD_RECACHE_BOOK,
};
constexpr size_t kDetailLineBytes = 192;

// The bounded FUI row window adds a small amount of resident state, but keeps
// large shelves off the heap. Keep a guard so future additions do not turn
// this stateful network page into an unbounded activity object.
static_assert(sizeof(WeReadBrowseActivity) <= 6 * 1024, "WeRead browse activity exceeds its fixed heap budget");

}  // namespace

void WeReadBrowseActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_FUI_ROW, &WeReadBrowseActivity::onFuiRow, this);
  app.setScreen(&WeReadBrowseActivity::fuiScreen, this);
  fuiNav_.reset();
  // The browse page contains book/user supplied Chinese text just like the
  // file browser. Load the selected SD face before the first FUI build so the
  // menu and list rows never get measured with the bundled Latin-only face.
  sdFontSystem.ensureLoaded(renderer);
  const int listFontId = DynamicFont::fontForSdCardText(renderer, UI_12_FONT_ID);
  LOG_INF("WRB", "Browse UI font source=%s id=%d",
          renderer.isSdCardFont(listFontId) ? "SD" : "builtin", listFontId);
  operation_.reset();
  wifiReleasePending_ = false;
  if (reloadCache()) {
    state_ = State::Menu;
    requestUpdate();
  } else {
    connectThenCache();
  }
}

void WeReadBrowseActivity::onExit() {
  closeFuiRouting();
  for (int i = 0; i < kMaxFuiWindow; ++i) {
    fuiWindowLabels_[i].clear();
    fuiWindowSubtitles_[i].clear();
    fuiWindowItems_[i] = {};
  }
  operation_.reset();
  closePage();
  // ActivityManager invokes onExit() while holding RenderLock.
  releaseReaderFont();
  Activity::onExit();
}

void WeReadBrowseActivity::fuiScreen(UiScreen& screen, void* user) {
  static_cast<WeReadBrowseActivity*>(user)->buildFuiScreen(screen);
}

void WeReadBrowseActivity::onFuiRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WeReadBrowseActivity*>(user);
  if (event.value < 0) return;

  if (self->state_ == State::Menu) {
    if (event.value >= kMenuCount) return;
    self->menuSelected_ = event.value;
    self->fuiNav_.selected = event.value;
    self->app.clearTapFlash();
    self->closeFuiRouting();
    self->activateMenu();
  } else if (self->state_ == State::List) {
    const int count = self->pageItemCount();
    if (event.value >= count) return;
    self->listSelected_ = event.value;
    self->fuiNav_.selected = event.value;
    self->app.clearTapFlash();
    self->closeFuiRouting();
    self->activateList();
  }
}

void WeReadBrowseActivity::buildFuiScreen(UiScreen& screen) {
  if (state_ != State::Menu && state_ != State::List) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  // FUI text styles refer to font slots, not concrete GfxRenderer IDs. Bind
  // both title and subtitle slots to the SD face for this page, matching the
  // FileBrowser/RecentBooks list contract for mixed Latin/CJK strings.
  const int bodyFontId = DynamicFont::fontForSdCardText(renderer, UI_12_FONT_ID);
  const int smallFontId = DynamicFont::fontForSdCardText(renderer, UI_10_FONT_ID);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, bodyFontId);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, smallFontId);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool listState = state_ == State::List;
  const int count = listState ? pageItemCount() : kMenuCount;
  fuiStateCount_ = count;
  if (count <= 0) {
    screen.centeredText(tr(STR_WEREAD_BROWSE_EMPTY), screen.theme().bodyText);
    return;
  }

  int selected = listState ? listSelected_ : menuSelected_;
  selected = std::clamp(selected, 0, count - 1);
  if (listState) listSelected_ = selected;
  else menuSelected_ = selected;

  fuiListProps_ = {};
  fuiListProps_.count = static_cast<uint16_t>(count);
  fuiListProps_.action = ACTION_FUI_ROW;
  fuiListProps_.inputMask = fui::InputTouch;
  fuiListProps_.labelText = screen.theme().bodyText;
  if (listState) {
    fuiListProps_.subtitleText = screen.theme().smallText;
    fuiListProps_.valueText = screen.theme().smallText;
  }
  fuiListProps_.valueInset = 8;
  const int16_t rowHeight = mappedInput.hasTouch()
                                ? screen.theme().rowHeight
                                : static_cast<int16_t>(listState ? metrics.listWithSubtitleRowHeight
                                                                  : metrics.listRowHeight);
  fuiListProps_.rowHeight = rowHeight;
  fuiNav_.selected = selected;
  fuiNav_.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, count, fuiListProps_);

  const int first = std::clamp(fuiNav_.top, 0, std::max(0, count - 1));
  const int windowCount = std::min({fuiNav_.visibleRows, count - first, kMaxFuiWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = first + i;
    if (listState) {
      fuiWindowLabels_[i] = rowTitle(index);
      fuiWindowSubtitles_[i] = rowSubtitle(index);
    } else {
      fuiWindowLabels_[i] = I18N.get(kMenuTitles[index]);
      fuiWindowSubtitles_[i].clear();
    }
    fuiWindowItems_[i] = {};
    fuiWindowItems_[i].label = fuiWindowLabels_[i].c_str();
    fuiWindowItems_[i].subtitle = fuiWindowSubtitles_[i].empty() ? nullptr : fuiWindowSubtitles_[i].c_str();
    fuiWindowItems_[i].actionValue = static_cast<int16_t>(index);
  }
  if (renderer.isSdCardFont(bodyFontId)) {
    const uint8_t labelStyleMask = screen.theme().bodyText.bold ? 0x02 : 0x01;
    for (int i = 0; i < windowCount; ++i) {
      const int index = first + i;
      const int titleMissed = DynamicFont::prewarmIfSdFont(renderer, bodyFontId, fuiWindowLabels_[i], labelStyleMask);
      const int subtitleMissed = DynamicFont::prewarmIfSdFont(renderer, smallFontId, fuiWindowSubtitles_[i]);
      if (titleMissed > 0 || subtitleMissed > 0) {
        LOG_INF("WRB", "row=%d SD glyph miss: titleBytes=%u missed=%d subtitleBytes=%u missed=%d",
                index, static_cast<unsigned>(fuiWindowLabels_[i].size()), titleMissed,
                static_cast<unsigned>(fuiWindowSubtitles_[i].size()), subtitleMissed);
      }
    }
    DynamicFont::prewarmIfSdFont(renderer, bodyFontId, "\xe2\x80\xa6", labelStyleMask);
  }
  fuiListProps_.items = fuiWindowItems_;
  fuiListProps_.itemsWindowFirst = static_cast<uint16_t>(first);
  fuiListProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  screen.list(fuiListProps_);
}

bool WeReadBrowseActivity::routeFuiTouch() {
  if (state_ != State::Menu && state_ != State::List) return false;
  const auto touch = routeTouch(mappedInput);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    // A raw release (for example the end of a swipe) has no FUI action and
    // must fall through to the swipe classifier below.
    if (touch.event) return true;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::Up && swipe != MappedInputManager::SwipeDir::Down) return false;
  const int count = state_ == State::Menu ? kMenuCount : pageItemCount();
  const int delta = swipe == MappedInputManager::SwipeDir::Up ? fuiNav_.pageRows() : -fuiNav_.pageRows();
  if (fuiNav_.scrollBy(delta, count)) requestUpdate();
  return true;
}

bool WeReadBrowseActivity::preventAutoSleep() { return state_ == State::Loading; }

Rect WeReadBrowseActivity::contentBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect content = SubpageLayout::contentRect(screen, metrics);
  return Rect{content.x, content.y, content.width, std::max(1, content.height)};
}

int WeReadBrowseActivity::pageItemCount() const {
  const bool hasNext = currentPage_ + 1 < cache_.pageCounts[WeReadBrowse::kindIndex(kind_)];
  return static_cast<int>(pageHeader_.count) + (hasNext ? 1 : 0);
}

const char* WeReadBrowseActivity::kindTitle() const {
  switch (kind_) {
    case WeReadBrowse::Kind::PopularHighlights:
      return tr(STR_WEREAD_BROWSE_POPULAR_HIGHLIGHTS);
    case WeReadBrowse::Kind::MyHighlights:
      return tr(STR_WEREAD_BROWSE_MY_HIGHLIGHTS);
    case WeReadBrowse::Kind::PopularReviews:
      return tr(STR_WEREAD_BROWSE_POPULAR_REVIEWS);
  }
  return tr(STR_WEREAD_BROWSE_ENTRY);
}

bool WeReadBrowseActivity::reloadCache() {
  WeReadStore::Session session;
  if (!WeReadStore::loadSession(session) || !WeReadBrowse::loadCache(book_.bookId, session.vid, cache_)) {
    cache_ = {};
    hasCache_ = false;
    return false;
  }
  hasCache_ = true;
  return true;
}

void WeReadBrowseActivity::closePage() {
  if (index_.isOpen()) index_.close();
  if (text_.isOpen()) text_.close();
  pageHeader_ = {};
}

bool WeReadBrowseActivity::openPage(const uint32_t page) {
  closePage();
  if (!WeReadBrowse::openPage(book_.bookId, cache_, kind_, page, pageHeader_, index_, text_)) return false;
  currentPage_ = page;
  const int count = pageItemCount();
  listSelected_ = count > 0 ? std::min(listSelected_, count - 1) : 0;
  return true;
}

bool WeReadBrowseActivity::readRecord(const uint32_t index, WeReadBrowse::Record& record) {
  return WeReadBrowse::readRecord(index_, pageHeader_, index, record);
}

std::string WeReadBrowseActivity::rowTitle(const int index) {
  if (index == static_cast<int>(pageHeader_.count)) return tr(STR_NEXT_PAGE);
  WeReadBrowse::Record record;
  if (index < 0 || !readRecord(static_cast<uint32_t>(index), record) || !text_.seek(record.textOffset)) return {};
  char preview[160] = {};
  size_t length = 0;
  uint32_t consumed = 0;
  while (consumed < record.textLength && length + 4 < sizeof(preview)) {
    Utf8Glyph glyph;
    if (!readUtf8Glyph(text_, record.textLength - consumed, glyph)) break;
    consumed += glyph.fileBytes;
    if (glyph.text[0] == '\n') break;
    memcpy(preview + length, glyph.text, glyph.textBytes);
    length += glyph.textBytes;
  }
  preview[length] = '\0';
  return preview;
}

std::string WeReadBrowseActivity::rowSubtitle(const int index) {
  if (index < 0 || index >= static_cast<int>(pageHeader_.count)) return {};
  WeReadBrowse::Record record;
  if (!readRecord(static_cast<uint32_t>(index), record)) return {};
  char result[192] = {};
  size_t used = 0;
  const auto append = [&result, &used](const char* value) {
    if (!value || !value[0] || used + 1 >= sizeof(result)) return;
    const int length = snprintf(result + used, sizeof(result) - used, "%s%s", used == 0 ? "" : " · ", value);
    if (length > 0) used = std::min(sizeof(result) - 1, used + static_cast<size_t>(length));
  };
  append(record.chapter);
  append(record.author);
  if (record.heat > 0) {
    char heat[48];
    snprintf(heat, sizeof(heat), tr(STR_WEREAD_BROWSE_HEAT_FMT), static_cast<unsigned>(record.heat));
    append(heat);
  }
  if (record.rating > 0) {
    char rating[48];
    const float normalized =
        std::min(5.0f, record.rating > 5 ? record.rating / 10.0f : static_cast<float>(record.rating));
    snprintf(rating, sizeof(rating), tr(STR_WEREAD_BROWSE_RATING_FMT), normalized);
    append(rating);
  }
  return result;
}

void WeReadBrowseActivity::connectThenCache() {
  if (readerFontReady_) {
    RenderLock renderBarrier(*this);
    releaseReaderFont();
  }
  if (WiFi.status() == WL_CONNECTED) {
    startCache();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    error_ = WeReadClient::Error::OutOfMemory;
    state_ = State::Error;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    wifiReleasePending_ = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                          mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                          mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
                          mappedInput.isPressed(MappedInputManager::Button::NavNext);
    if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
      startCache();
    } else {
      error_ = WeReadClient::Error::Network;
      state_ = State::Error;
      requestUpdate();
    }
  });
}

void WeReadBrowseActivity::startCache() {
  closeFuiRouting();
  closePage();
  qrReady_ = false;
  error_ = WeReadClient::Error::Ok;
  if (!operation_.beginBrowseCache(book_)) {
    error_ = operation_.error();
    state_ = State::Error;
  } else {
    state_ = State::Loading;
  }
  requestUpdate();
}

void WeReadBrowseActivity::stepLoad() {
  WeReadClient::Operation::Event event;
  {
    RenderLock renderBarrier(*this);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
    event = operation_.step();
  }
  switch (event) {
    case WeReadClient::Operation::Event::None:
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterRangeReady:
    case WeReadClient::Operation::Event::ChapterComplete:
      return;
    case WeReadClient::Operation::Event::QrReady:
      qrReady_ = true;
      requestUpdate();
      return;
    case WeReadClient::Operation::Event::Authenticated:
      qrReady_ = false;
      requestUpdate();
      return;
    case WeReadClient::Operation::Event::Complete:
      operation_.reset();
      if (!reloadCache()) {
        error_ = WeReadClient::Error::SdCard;
        state_ = State::Error;
      } else {
        menuSelected_ = 0;
        listSelected_ = 0;
        state_ = State::Menu;
      }
      requestUpdate();
      return;
    case WeReadClient::Operation::Event::Cancelled:
      if (reloadCache()) {
        state_ = State::Menu;
        requestUpdate();
      } else {
        finish();
      }
      return;
    case WeReadClient::Operation::Event::Failed:
      error_ = operation_.error();
      reloadCache();
      state_ = State::Error;
      requestUpdate();
      return;
  }
}

void WeReadBrowseActivity::activateMenu() {
  switch (static_cast<MenuAction>(menuSelected_)) {
    case MenuAction::PopularHighlights:
      kind_ = WeReadBrowse::Kind::PopularHighlights;
      break;
    case MenuAction::MyHighlights:
      kind_ = WeReadBrowse::Kind::MyHighlights;
      break;
    case MenuAction::PopularReviews:
      kind_ = WeReadBrowse::Kind::PopularReviews;
      break;
    case MenuAction::Refresh:
      connectThenCache();
      return;
  }
  listSelected_ = 0;
  closeFuiRouting();
  fuiNav_.reset();
  if (!openPage(0)) {
    hasCache_ = false;
    connectThenCache();
    return;
  }
  state_ = State::List;
  requestUpdate();
}

void WeReadBrowseActivity::activateList() {
  if (listSelected_ == static_cast<int>(pageHeader_.count)) {
    closeFuiRouting();
    fuiNav_.reset();
    listSelected_ = 0;
    if (!openPage(currentPage_ + 1)) {
      error_ = WeReadClient::Error::SdCard;
      state_ = State::Error;
    }
    requestUpdate();
    return;
  }
  if (!readRecord(static_cast<uint32_t>(listSelected_), selectedRecord_)) {
    closeFuiRouting();
    error_ = WeReadClient::Error::SdCard;
    state_ = State::Error;
    requestUpdate();
    return;
  }
  openDetail();
}

void WeReadBrowseActivity::releaseReaderFont() {
  if (!readerFontReady_) return;
  sdFontSystem.releaseLoadedFont(renderer);
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  readerFontId_ = 0;
  readerFontReady_ = false;
}

void WeReadBrowseActivity::openDetail() {
  closeFuiRouting();
  {
    RenderLock renderBarrier(*this);
    if (!readerFontReady_) {
      sdFontSystem.ensureLoaded(renderer);
      // Browse detail text follows the same SD dynamic-font policy as the
      // file browser. Keep the configured reader font as a fallback for
      // devices without an SD font, but prefer the loaded SD face whenever it
      // is available so CJK highlights/reviews remain complete.
      readerFontId_ = DynamicFont::fontForSdCardText(renderer, SETTINGS.getReaderFontId());
      LOG_INF("WRB", "Browse detail font source=%s id=%d",
              renderer.isSdCardFont(readerFontId_) ? "SD" : "builtin", readerFontId_);
      readerFontReady_ = true;
    }
    buildTextPages();
  }
  textPage_ = 0;
  state_ = State::Detail;
  requestUpdate();
}

void WeReadBrowseActivity::buildTextPages() {
  textPageOffsets_[0] = 0;
  textPageCount_ = 0;
  textPagesTruncated_ = false;
  const Rect content = contentBounds();
  const int lineHeight = renderer.getLineHeight(readerFontId_);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int footerFontId = DynamicFont::fontForSdCardText(renderer, SMALL_FONT_ID);
  const int footerHeight = renderer.getLineHeight(footerFontId) + metrics.verticalSpacing;
  const int maxLines = std::max(1, (content.height - footerHeight) / lineHeight);
  const int maxWidth = std::max(1, content.width - metrics.contentSidePadding * 2);
  uint32_t offset = 0;
  while (offset < selectedRecord_.textLength && textPageCount_ < kMaxTextPages) {
    const uint32_t pageStart = offset;
    int lines = 0;
    int lineWidth = 0;
    size_t lineLength = 0;
    if (!text_.seek(selectedRecord_.textOffset + pageStart)) break;
    while (offset < selectedRecord_.textLength && lines < maxLines) {
      const uint32_t glyphStart = offset;
      Utf8Glyph glyph;
      if (!readUtf8Glyph(text_, selectedRecord_.textLength - offset, glyph)) break;
      offset += glyph.fileBytes;
      if (glyph.text[0] == '\n') {
        ++lines;
        lineWidth = 0;
        lineLength = 0;
        continue;
      }
      const int width = renderer.getTextAdvanceX(readerFontId_, glyph.text, EpdFontFamily::REGULAR);
      if ((lineWidth > 0 && lineWidth + width > maxWidth) || lineLength + glyph.textBytes >= kDetailLineBytes) {
        ++lines;
        lineWidth = 0;
        lineLength = 0;
        if (lines >= maxLines) {
          offset = glyphStart;
          break;
        }
      }
      lineWidth += width;
      lineLength += glyph.textBytes;
    }
    if (offset <= pageStart) break;
    ++textPageCount_;
    textPageOffsets_[textPageCount_] = offset;
  }
  if (textPageCount_ == 0) {
    textPageCount_ = 1;
    textPageOffsets_[1] = selectedRecord_.textLength;
  }
  textPagesTruncated_ = offset < selectedRecord_.textLength;
}

void WeReadBrowseActivity::drawDetail(const Rect& content) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int side = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(readerFontId_);
  const int footerFontId = DynamicFont::fontForSdCardText(renderer, SMALL_FONT_ID);
  const int footerY = content.y + content.height - renderer.getLineHeight(footerFontId);
  const uint32_t start = textPageOffsets_[textPage_];
  const uint32_t end = textPageOffsets_[textPage_ + 1];
  const int maxWidth = content.width - side * 2;
  const auto drawBody = [&]() {
    if (!text_.seek(selectedRecord_.textOffset + start)) return false;
    char line[kDetailLineBytes] = {};
    size_t lineLength = 0;
    int lineWidth = 0;
    int y = content.y;
    uint32_t offset = start;
    const auto flushLine = [&]() {
      line[lineLength] = '\0';
      if (lineLength > 0) renderer.drawText(readerFontId_, content.x + side, y, line);
      lineLength = 0;
      lineWidth = 0;
      y += lineHeight;
    };
    while (offset < end && y < footerY) {
      Utf8Glyph glyph;
      if (!readUtf8Glyph(text_, end - offset, glyph)) break;
      offset += glyph.fileBytes;
      if (glyph.text[0] == '\n') {
        flushLine();
        continue;
      }
      const int width = renderer.getTextAdvanceX(readerFontId_, glyph.text, EpdFontFamily::REGULAR);
      if ((lineWidth > 0 && lineWidth + width > maxWidth) || lineLength + glyph.textBytes >= sizeof(line)) flushLine();
      memcpy(line + lineLength, glyph.text, glyph.textBytes);
      lineLength += glyph.textBytes;
      lineWidth += width;
    }
    if (lineLength > 0 && y < footerY) flushLine();
    return true;
  };

  bool bodyReady = true;
  auto* fontCache = renderer.getFontCacheManager();
  if (fontCache != nullptr && fontCache->needsPrewarmScan(readerFontId_)) {
    auto prewarmScope = fontCache->createPrewarmScope();
    bodyReady = drawBody();
    if (bodyReady) {
      prewarmScope.endScanAndPrewarm();
      bodyReady = drawBody();
    }
  } else {
    bodyReady = drawBody();
  }
  if (!bodyReady) {
    GUI.drawPopup(renderer, tr(STR_WEREAD_DETAIL_UNAVAILABLE));
    return;
  }

  char page[48];
  snprintf(page, sizeof(page), tr(STR_WEREAD_PAGE_FMT), static_cast<unsigned>(textPage_ + 1),
           static_cast<unsigned>(textPageCount_));
  const bool truncated = textPage_ + 1 == textPageCount_ &&
                         ((selectedRecord_.flags & WeReadBrowse::kRecordTextTruncated) != 0 || textPagesTruncated_);
  renderer.drawCenteredText(footerFontId, footerY, truncated ? tr(STR_WEREAD_BROWSE_TRUNCATED) : page);
}

void WeReadBrowseActivity::handleMenuInput() {
  navigator_.onNextRelease([this] {
    menuSelected_ = ButtonNavigator::nextIndex(menuSelected_, kMenuCount);
    fuiNav_.selected = menuSelected_;
    fuiNav_.follow(kMenuCount);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this] {
    menuSelected_ = ButtonNavigator::previousIndex(menuSelected_, kMenuCount);
    fuiNav_.selected = menuSelected_;
    fuiNav_.follow(kMenuCount);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateMenu();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void WeReadBrowseActivity::handleListInput() {
  const int count = pageItemCount();
  if (count <= 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state_ = State::Menu;
      requestUpdate();
    }
    return;
  }
  const int pageItems = std::max(1, fuiNav_.pageRows());
  navigator_.onNextRelease([this, count] {
    listSelected_ = ButtonNavigator::nextIndex(listSelected_, count);
    fuiNav_.selected = listSelected_;
    fuiNav_.follow(count);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, count] {
    listSelected_ = ButtonNavigator::previousIndex(listSelected_, count);
    fuiNav_.selected = listSelected_;
    fuiNav_.follow(count);
    requestUpdate();
  });
  navigator_.onNextContinuous([this, count, pageItems] {
    listSelected_ = ButtonNavigator::nextPageIndex(listSelected_, count, pageItems);
    fuiNav_.selected = listSelected_;
    fuiNav_.follow(count);
    requestUpdate();
  });
  navigator_.onPreviousContinuous([this, count, pageItems] {
    listSelected_ = ButtonNavigator::previousPageIndex(listSelected_, count, pageItems);
    fuiNav_.selected = listSelected_;
    fuiNav_.follow(count);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateList();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (currentPage_ > 0 && openPage(currentPage_ - 1)) {
      listSelected_ = std::max(0, pageItemCount() - 1);
      fuiNav_.selected = listSelected_;
      fuiNav_.follow(pageItemCount());
    } else {
      closePage();
      closeFuiRouting();
      fuiNav_.reset();
      state_ = State::Menu;
    }
    requestUpdate();
  }
}

void WeReadBrowseActivity::handleDetailInput() {
  const auto next = [this] {
    if (textPage_ + 1 < textPageCount_) {
      ++textPage_;
      requestUpdate();
    }
  };
  const auto previous = [this] {
    if (textPage_ > 0) {
      --textPage_;
      requestUpdate();
    }
  };
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    next();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    previous();
    return;
  }
  navigator_.onNextRelease(next);
  navigator_.onPreviousRelease(previous);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state_ = State::List;
    requestUpdate();
  }
}

void WeReadBrowseActivity::handleErrorInput() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    connectThenCache();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    operation_.reset();
    if (hasCache_) {
      state_ = State::Menu;
      requestUpdate();
    } else {
      finish();
    }
  }
}

const char* WeReadBrowseActivity::errorMessage() const {
  switch (error_) {
    case WeReadClient::Error::Network:
      return WiFi.status() == WL_CONNECTED ? tr(STR_WEREAD_HTTP_ERROR) : tr(STR_WEREAD_NO_WIFI);
    case WeReadClient::Error::SessionExpired:
    case WeReadClient::Error::LoginFailed:
      return tr(STR_WEREAD_LOGIN_REQUIRED);
    case WeReadClient::Error::SdCard:
    case WeReadClient::Error::Integrity:
      return tr(STR_WEREAD_DETAIL_UNAVAILABLE);
    case WeReadClient::Error::Ok:
    case WeReadClient::Error::Cancelled:
    case WeReadClient::Error::Protocol:
    case WeReadClient::Error::Unavailable:
    case WeReadClient::Error::Clock:
    case WeReadClient::Error::OutOfMemory:
    case WeReadClient::Error::WholeBookOnly:
      return tr(STR_WEREAD_HTTP_ERROR);
  }
  return tr(STR_WEREAD_HTTP_ERROR);
}

void WeReadBrowseActivity::loop() {
  if (wifiReleasePending_) {
    const bool held = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                      mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
                      mappedInput.isPressed(MappedInputManager::Button::NavNext);
    if (!held) wifiReleasePending_ = false;
    return;
  }

  if (routeFuiTouch()) return;

  switch (state_) {
    case State::Menu:
      handleMenuInput();
      break;
    case State::Loading:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        operation_.cancel();
      } else {
        stepLoad();
      }
      break;
    case State::List:
      handleListInput();
      break;
    case State::Detail:
      handleDetailInput();
      break;
    case State::Error:
      handleErrorInput();
      break;
  }
}

void WeReadBrowseActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* header = state_ == State::List || state_ == State::Detail ? kindTitle() : tr(STR_WEREAD_BROWSE_ENTRY);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight}, header);
  const Rect content = contentBounds();
  switch (state_) {
    case State::Menu:
      renderUi();
      break;
    case State::Loading:
      if (qrReady_) {
        const int fontId = DynamicFont::fontForSdCardText(renderer, UI_10_FONT_ID);
        const int lineHeight = renderer.getLineHeight(fontId);
        const int qrSide = std::max(1, std::min(content.width * 4 / 5, content.height - lineHeight * 2));
        const int qrY = content.y + std::max(0, (content.height - qrSide - lineHeight) / 2);
        QrUtils::drawQrCode(renderer, Rect{content.x + (content.width - qrSide) / 2, qrY, qrSide, qrSide},
                            operation_.qrUrl());
        renderer.drawCenteredText(fontId, qrY + qrSide, tr(STR_WEREAD_SCAN_LOGIN));
      } else {
        GUI.drawPopup(renderer, tr(STR_WEREAD_CACHING));
      }
      break;
    case State::List: {
      if (pageHeader_.count == 0 && pageItemCount() == 0) {
        GUI.drawPopup(renderer, tr(STR_WEREAD_BROWSE_EMPTY));
      } else {
        renderUi();
      }
      const bool reviewsLimited = kind_ == WeReadBrowse::Kind::PopularReviews &&
                                  currentPage_ + 1 == cache_.pageCounts[WeReadBrowse::kindIndex(kind_)] &&
                                  (cache_.flags & WeReadBrowse::kCacheReviewsLimited) != 0;
      if (reviewsLimited || (pageHeader_.flags & WeReadBrowse::kPageResponseTruncated) != 0) {
        const int footerFontId = DynamicFont::fontForSdCardText(renderer, SMALL_FONT_ID);
        renderer.drawCenteredText(
            footerFontId, content.y + content.height - renderer.getLineHeight(footerFontId),
            I18N.get(reviewsLimited ? StrId::STR_WEREAD_BROWSE_LIMITED : StrId::STR_WEREAD_BROWSE_TRUNCATED));
      }
      break;
    }
    case State::Detail:
      drawDetail(content);
      break;
    case State::Error:
      GUI.drawPopup(renderer, errorMessage());
      break;
  }

  const char* back = state_ == State::Loading ? tr(STR_CANCEL) : tr(STR_BACK);
  const char* confirm = "";
  const char* previous = "";
  const char* next = "";
  if (state_ == State::Menu || state_ == State::List) {
    confirm = tr(STR_SELECT);
    previous = tr(STR_DIR_UP);
    next = tr(STR_DIR_DOWN);
  } else if (state_ == State::Detail) {
    previous = tr(STR_DIR_UP);
    next = tr(STR_DIR_DOWN);
  } else if (state_ == State::Error) {
    confirm = tr(STR_RETRY);
  }
  const auto labels = mappedInput.mapLabels(back, confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
