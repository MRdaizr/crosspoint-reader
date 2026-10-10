#include "LibraryListActivity.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

library::SortOrder LibraryListActivity::order() const {
  const bool desc = descending & (1u << tab);
  if (tab == 1) return desc ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == 2) return desc ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return desc ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}
const char* LibraryListActivity::tabLabel(const int which) const {
  if (which == 1) return tr(STR_LIBRARY_TAB_TITLE);
  if (which == 2) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}
const char* LibraryListActivity::headerTitle() const { return header[0] ? header : tr(STR_LIBRARY); }
bool LibraryListActivity::hasPins() const { return tab == 0 && (descending & 1u) && !query[0]; }
int LibraryListActivity::pinnedCount() const { return hasPins() ? pins : 0; }
int LibraryListActivity::listCount() const { return TOOLS + matched + (hasPins() ? pins - overlaps : 0); }

void LibraryListActivity::onEnter() {
  RenderLock lock(*this);
  // Checked page storage (~16 KiB), never proportional to the shelf. Strings
  // remain fixed buffers so scrolling does not allocate once per title.
  queryEngine = makeUniqueNoThrow<library::LibraryQuery>();
  rows = makeUniqueNoThrow<DisplayRow[]>(library::LIBRARY_PAGE_LIMIT);
  if (!queryEngine || !rows) {
    LOG_ERR("LIB", "OOM: query/page");
    dataFailed = true;
  }
  UiTabListActivity::onEnter();
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  const bool recovered = library::recoverLibraryIndex();
  const bool opened = recovered && index.open(library::libraryIndexPath());
  rebuildPending =
      !opened || library::isLibraryIndexDirty() || index.header().metadataEnabled != (SETTINGS.libraryUseMetadata != 0);
  if (!dataFailed && opened) {
    refreshQuery();
    resolvePinned();
  }
  if (rebuildPending) status = StrId::STR_LIBRARY_REBUILDING;
  swallowConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void LibraryListActivity::onExit() {
  UiListActivity::onExit();  // close routing before releasing provider buffers
  index.close();
  rows.reset();
  queryEngine.reset();
}

void LibraryListActivity::invalidateWindow() {
  windowFirst = -1;
  windowCount = 0;
  invalidateListFontPrewarm();
}
void LibraryListActivity::refreshQuery() {
  invalidateWindow();
  matched = 0;
  if (!queryEngine || !index.isOpen()) return;
  dataFailed = !queryEngine->run(index, query, order(), 0, 1, page);
  if (!dataFailed) matched = page.total;
  snprintf(header, sizeof(header), "%s%s%s", tr(STR_LIBRARY), query[0] ? ": " : "", query);
}

void LibraryListActivity::resolvePinned() {
  pins = overlaps = 0;
  const auto& books = RECENT_BOOKS.getBooks();
  if (!index.isOpen()) return;
  pins = std::min<size_t>(books.size(), MAX_PINNED);
  // Small, checked scratch instead of 16-byte identities on the task stack.
  auto identities = makeUniqueNoThrow<library::BookIdentity[]>(pins ? pins : 1);
  if (!identities) {
    LOG_ERR("LIB", "OOM: recent identities");
    pins = 0;
    return;
  }
  for (uint8_t i = 0; i < pins; ++i) {
    const auto& raw = books[i].path;
    identities[i] = {library::clixPathHash(raw.data(), raw.size()), 0};
  }
  if (!index.recentRowsFor(identities.get(), pins, pinnedAsc)) {
    pins = 0;
    return;
  }
  for (uint8_t i = 0; i < pins; ++i) {
    if (pinnedAsc[i] != 0xFFFF) overlap[overlaps++] = index.bookCount() - 1 - pinnedAsc[i];
  }
  std::sort(overlap, overlap + overlaps);
}

uint16_t LibraryListActivity::ordinalForBook(int row) {
  if (row < pinnedCount()) return 0xFFFF;
  if (query[0]) {
    if (!queryEngine->run(index, query, order(), row, 1, page) || !page.count) return 0xFFFF;
    return page.ordinals[0];
  }
  row -= pinnedCount();
  if (hasPins())
    for (uint8_t i = 0; i < overlaps; ++i)
      if (overlap[i] <= row) ++row;
  return row < index.bookCount() ? index.ordinalForRow(order(), row) : 0xFFFF;
}

bool LibraryListActivity::loadWindow(const int first, const int count) {
  if (windowFirst == first && windowCount == count && windowOrder == order()) return true;
  if (!rows || !queryEngine) return false;
  const int firstBook = std::max(0, first - TOOLS);
  if (query[0] && !queryEngine->run(index, query, order(), firstBook, library::LIBRARY_PAGE_LIMIT, page)) return false;
  for (int i = 0; i < count; ++i) {
    auto& row = rows[i];
    row.title[0] = row.author[0] = '\0';
    const int absolute = first + i;
    if (absolute < TOOLS) continue;
    const int book = absolute - TOOLS;
    if (book < pinnedCount()) {
      const auto& recent = RECENT_BOOKS.getBooks()[book];
      snprintf(row.title, sizeof(row.title), "%s", recent.title.c_str());
      snprintf(row.author, sizeof(row.author), "%s", recent.author.c_str());
      row.title[utf8SafeTruncateBuffer(row.title, strlen(row.title))] = '\0';
      row.author[utf8SafeTruncateBuffer(row.author, strlen(row.author))] = '\0';
    } else {
      const uint16_t ordinal = query[0] ? page.ordinals[book - firstBook] : ordinalForBook(book);
      library::ClixRecord record{};
      if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) ||
          !queryEngine->readDisplay(index, record, row.title, sizeof(row.title), row.author, sizeof(row.author)))
        return false;
    }
    // Compose the display copy only. Opening keeps the indexed raw FAT path.
    utf8ComposeNfcInPlace(row.title);
    utf8ComposeNfcInPlace(row.author);
  }
  windowFirst = first;
  windowCount = count;
  windowOrder = order();
  invalidateListFontPrewarm();
  return true;
}

void LibraryListActivity::provideRow(void* context, const uint16_t absolute, fui::ListItem& item) {
  auto* self = static_cast<LibraryListActivity*>(context);
  item.actionValue = absolute;
  if (absolute == 0) {
    item.label = tr(STR_LIBRARY_SEARCH);
    return;
  }
  if (absolute == 1) {
    item.label = tr(STR_LIBRARY_REBUILD);
    return;
  }
  const int local = absolute - self->windowFirst;
  if (local < 0 || local >= self->windowCount) return;
  item.label = self->rows[local].title;
  item.subtitle = self->rows[local].author[0] ? self->rows[local].author : tr(STR_LIBRARY_UNKNOWN_AUTHOR);
}
const char* LibraryListActivity::prewarmText(const void* context, const uint32_t slot) {
  const auto* self = static_cast<const LibraryListActivity*>(context);
  const uint32_t row = slot / 2;
  if (row >= static_cast<uint32_t>(self->windowCount)) return "";
  return slot & 1u ? self->rows[row].author : self->rows[row].title;
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(std::max(top, metrics.topPadding + metrics.headerHeight)), static_cast<int16_t>(right),
      static_cast<int16_t>(std::max(bottom, metrics.buttonHintsHeight)), static_cast<int16_t>(left)});
  buildTabBar(screen);
  if (rebuildPending || rebuilding || dataFailed || !index.isOpen()) {
    screen.centeredText(dataFailed ? tr(STR_LIBRARY_READ_FAILED) : I18N.get(status), screen.theme().bodyText);
    return;
  }
  const char* warning = index.ranksDegraded()          ? tr(STR_LIBRARY_SORT_DEGRADED)
                        : status != StrId::STR_LIBRARY ? I18N.get(status)
                                                       : nullptr;
  if (warning) {
    const auto band = screen.takeTop(screen.target().lineHeight(screen.theme().smallText.font));
    screen.target().text(band, warning, screen.theme().smallText);
  }
  auto& props = listProps;
  props.rowProvider = &LibraryListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.subtitleText = screen.theme().smallText;
  syncTabListViewport(screen, props, true);
  const int first = activeNav().top;
  const int count =
      std::min({listCount() - first, activeNav().visibleRows, static_cast<int>(library::LIBRARY_PAGE_LIMIT)});
  if (!loadWindow(first, count)) {
    dataFailed = true;
    screen.centeredText(tr(STR_LIBRARY_READ_FAILED));
    return;
  }
  const int bodyFont = DynamicFont::fontForSdCardText(renderer, UI_12_FONT_ID);
  const int smallFont = DynamicFont::fontForSdCardText(renderer, UI_10_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, bodyFont);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, smallFont);
  DynamicFont::prewarmIfSdFont(renderer, bodyFont, &LibraryListActivity::prewarmText, this, count * 2);
  renderer.prewarmFallbackText(bodyFont, &LibraryListActivity::prewarmText, this, count * 2);
  screen.list(props);
}

void LibraryListActivity::onTabAction(const int which) {
  RenderLock lock(*this);
  if (which < 0 || which >= tabCount()) return;
  if (which == tab) descending ^= 1u << tab;
  tab = which;
  refreshQuery();
  activeNav().selected = 0;
  activeNav().followOnBuild = true;
  app.clearTapFlash();
  requestUpdate();
}
void LibraryListActivity::stepTab(const int direction) { onTabAction((tab + direction + tabCount()) % tabCount()); }

bool LibraryListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (swallowBack) {
      swallowBack = false;
      return true;
    }
    if (query[0]) {
      RenderLock lock(*this);
      query[0] = '\0';
      refreshQuery();
      requestUpdate();
    } else
      onGoHome(HomeMenuItem::LIBRARY);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (swallowConfirm) {
      swallowConfirm = false;
      return true;
    }
    if (ringPos() == 0)
      onTabAction(tab);
    else
      activateIndex(ringPos() - 1);
    return true;
  }
  return false;
}
bool LibraryListActivity::handleCustomInput() {
  if (!rebuildPending) return false;
  rebuild();
  return true;
}
void LibraryListActivity::activateIndex(const int row) {
  if (row == 0) {
    openSearch();
    return;
  }
  if (row == 1) {
    promptRebuild();
    return;
  }
  std::string selected;
  {
    RenderLock lock(*this);
    if (row < TOOLS || row >= listCount()) return;
    const int book = row - TOOLS;
    if (book < pinnedCount())
      selected = RECENT_BOOKS.getBooks()[book].path;
    else {
      const auto ordinal = ordinalForBook(book);
      library::ClixRecord record{};
      if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path, sizeof(path)))
        return;
      selected = path;
    }
    app.clearTapFlash();
    index.close();
  }
  onSelectBook(selected);
}

void LibraryListActivity::openSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query,
                                                           library::LIBRARY_QUERY_BYTES);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    RenderLock lock(*this);
    if (!result.isCancelled)
      if (const auto* text = std::get_if<KeyboardResult>(&result.data)) {
        snprintf(query, sizeof(query), "%s", text->text.c_str());
        refreshQuery();
        activeNav().selected = 1;
        activeNav().followOnBuild = true;
      }
    swallowConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    swallowBack = mappedInput.isPressed(MappedInputManager::Button::Back);
  });
}
void LibraryListActivity::promptRebuild() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_LIBRARY_REBUILD),
                                                              tr(STR_LIBRARY_REBUILD_CONFIRM));
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (!result.isCancelled) rebuildPending = true;
    swallowConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    swallowBack = mappedInput.isPressed(MappedInputManager::Button::Back);
  });
}

bool LibraryListActivity::cancelBuild(void* context) {
  auto* self = static_cast<LibraryListActivity*>(context);
  self->mappedInput.update();
  if (!self->mappedInput.isPressed(MappedInputManager::Button::Back)) self->cancelArmed = true;
  return self->cancelArmed && self->mappedInput.isPressed(MappedInputManager::Button::Back);
}
void LibraryListActivity::buildProgress(void* context, const library::BuildPhase phase, const uint16_t completed,
                                        const uint16_t total) {
  auto* self = static_cast<LibraryListActivity*>(context);
  const uint32_t now = millis();
  if (phase == self->lastPhase && now - self->lastProgressMs < 750) return;
  self->lastPhase = phase;
  self->lastProgressMs = now;
  char text[96];
  // BaseTheme's popup draws one line. Keep the cancellation hint in the
  // logical Back footer instead of embedding an unsupported newline.
  if (total)
    snprintf(text, sizeof(text), "%s %u/%u", tr(STR_LIBRARY_REBUILDING), completed, total);
  else
    snprintf(text, sizeof(text), "%s %u", tr(STR_LIBRARY_REBUILDING), completed);
  const auto labels = self->mappedInput.mapLabels(tr(STR_LIBRARY_CANCEL_HINT), "", "", "");
  GUI.drawButtonHints(self->renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const auto popup = GUI.drawPopup(self->renderer, text);
  if (total) GUI.fillPopupProgress(self->renderer, popup, std::min<int>(100, completed * 100u / total));
}
void LibraryListActivity::rebuild() {
  RenderLock lock(*this);
  rebuildPending = false;
  rebuilding = true;
  index.close();
  cancelArmed = !mappedInput.isPressed(MappedInputManager::Button::Back);
  library::BuildStats stats;
  const library::BuildCallbacks callbacks{this, &LibraryListActivity::cancelBuild, &LibraryListActivity::buildProgress};
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0, callbacks);
  status = stats.cancelled ? StrId::STR_LIBRARY_REBUILD_CANCELLED
           : ok            ? StrId::STR_LIBRARY
                           : StrId::STR_LIBRARY_REBUILD_FAILED;
  if (stats.busy) status = StrId::STR_LIBRARY_BUSY;
  if (stats.capped && ok) status = StrId::STR_LIBRARY_CAPPED;
  rebuilding = false;
  if (!library::recoverLibraryIndex() || !index.open(library::libraryIndexPath())) {
    matched = pins = overlaps = 0;
  } else {
    refreshQuery();
    resolvePinned();
  }
  swallowBack = mappedInput.isPressed(MappedInputManager::Button::Back);
  swallowConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}
