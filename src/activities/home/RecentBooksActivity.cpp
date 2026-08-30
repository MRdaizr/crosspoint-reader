#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

struct RecentBooksTextContext {
  const std::vector<RecentBook>* books;
  int startIndex;
};

const char* recentBookTitleGetter(const void* context, uint32_t index) {
  const auto* ctx = static_cast<const RecentBooksTextContext*>(context);
  const size_t itemIndex = static_cast<size_t>(ctx->startIndex) + index;
  return itemIndex < ctx->books->size() ? (*ctx->books)[itemIndex].title.c_str() : nullptr;
}

const char* recentBookAuthorGetter(const void* context, uint32_t index) {
  const auto* ctx = static_cast<const RecentBooksTextContext*>(context);
  const size_t itemIndex = static_cast<size_t>(ctx->startIndex) + index;
  return itemIndex < ctx->books->size() ? (*ctx->books)[itemIndex].author.c_str() : nullptr;
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

void RecentBooksActivity::onEnter() {
  UiListActivity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  nav.selected = 0;
  sdFontSystem.ensureLoaded(renderer);
  const int listFontId = sdFontSystem.currentFontId();
  LOG_INF("RBA", "RecentBooks list font source=%s id=%d",
          renderer.isSdCardFont(listFontId) ? "SD" : "builtin", listFontId);
}

void RecentBooksActivity::onExit() {
  UiListActivity::onExit();
  recentBooks.clear();
}

bool RecentBooksActivity::handleCustomInput() {
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return true;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && nav.selected < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[nav.selected].path, recentBooks[nav.selected].title);
    return true;
  }
  return false;
}

void RecentBooksActivity::onRowLongPress(const int index) {
  if (index >= 0 && index < static_cast<int>(recentBooks.size())) {
    longPressFired = true;
    promptRemoveBook(recentBooks[static_cast<size_t>(index)].path, recentBooks[static_cast<size_t>(index)].title);
  }
}

bool RecentBooksActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressFired) {
      longPressFired = false;
      return true;
    }
    if (!recentBooks.empty() && nav.selected < static_cast<int>(recentBooks.size())) {
      activateIndex(nav.selected);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return true;
  }
  return UiListActivity::handleButtons();
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        nav.selected = 0;
      } else if (nav.selected >= recentBooks.size()) {
        nav.selected = static_cast<int>(recentBooks.size() - 1);
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(recentBooks.size())) return;
  app.clearTapFlash();
  nav.selected = index;
  LOG_DBG("RBA", "Selected recent book: %s", recentBooks[static_cast<size_t>(index)].path.c_str());
  onSelectBook(recentBooks[static_cast<size_t>(index)].path);
}

void RecentBooksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // FUI stores font references as slots. Reset the slots before inspecting
  // this list so a previous CJK page cannot leave an SD face bound for an
  // unrelated English-only repaint.
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, UI_10_FONT_ID);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (recentBooks.empty()) {
    const char* emptyMessage = tr(STR_NO_RECENT_BOOKS);
    const int emptyFontId = DynamicFont::fontForSdCardText(renderer, UI_12_FONT_ID);
    uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, emptyFontId);
    fui::TextStyle emptyStyle = screen.theme().bodyText;
    emptyStyle.bold = !renderer.isSdCardFont(emptyFontId);
    screen.centeredText(emptyMessage, emptyStyle);
    return;
  }
  rowLabels.clear(); rowSubtitles.clear(); rowItems.clear();
  rowLabels.reserve(recentBooks.size()); rowSubtitles.reserve(recentBooks.size()); rowItems.reserve(recentBooks.size());
  for (size_t i = 0; i < recentBooks.size(); ++i) {
    rowLabels.push_back(recentBooks[i].title);
    rowSubtitles.push_back(recentBooks[i].author);
    fui::ListItem item; item.label = rowLabels.back().c_str(); item.subtitle = rowSubtitles.back().empty() ? nullptr : rowSubtitles.back().c_str();
    item.actionValue = static_cast<int16_t>(i); item.icon = {}; rowItems.push_back(item);
  }

  // Recent-book rows use one shared FUI small-text slot for both title and
  // author.  Select the SD face for every row so English-only books and CJK
  // books follow the same dynamic-font path.
  const int listFontId = DynamicFont::fontForSdCardText(renderer, UI_10_FONT_ID);
  const bool usingSdFont = renderer.isSdCardFont(listFontId);
  uiTarget.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, listFontId);

  fui::ListProps props; props.items = rowItems.data(); props.count = static_cast<uint16_t>(rowItems.size()); props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.subtitleText = screen.theme().smallText; props.subtitleText.maxLines = 1;
  fui::TextStyle label = screen.theme().smallText;
  // FreeInkApp treats an all-default TextStyle as "unset" and substitutes
  // bodyText.  With an SD font, a regular (non-bold) small-text label would
  // otherwise look default and silently move from FONT_SMALL (the SD face)
  // to FONT_BODY (the bundled UI face), which drops CJK glyphs.  maxLines=0
  // still renders as one line in GfxRendererTarget, but marks this style as
  // caller-owned so the small-font slot remains authoritative.
  label.maxLines = 0;
  label.bold = !usingSdFont;
  props.labelText = label;
  syncListViewport(screen, props, true);
  if (usingSdFont) {
    const int first = std::clamp(nav.top, 0, static_cast<int>(rowLabels.size()));
    const int count = std::min(static_cast<int>(rowLabels.size()) - first, std::max(1, nav.visibleRows));
    for (int i = 0; i < count; ++i) {
      const size_t row = static_cast<size_t>(first + i);
      const int titleMissed = DynamicFont::prewarmIfSdFont(renderer, listFontId, rowLabels[row]);
      const int authorMissed = DynamicFont::prewarmIfSdFont(renderer, listFontId, rowSubtitles[row]);
      if (titleMissed > 0 || authorMissed > 0) {
        LOG_INF("RBA", "row=%d SD glyph miss: titleBytes=%u missed=%d authorBytes=%u missed=%d title='%s' author='%s'",
                first + i, static_cast<unsigned>(rowLabels[row].size()), titleMissed,
                static_cast<unsigned>(rowSubtitles[row].size()), authorMissed, rowLabels[row].c_str(),
                rowSubtitles[row].c_str());
      }
    }
    DynamicFont::prewarmIfSdFont(renderer, listFontId, "\xe2\x80\xa6");
  }
  screen.list(props);
}

void RecentBooksActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
