#include "UiListActivity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace fui = freeink::ui;

namespace {
struct VisibleRowsPrewarmContext {
  const char* (*getter)(const void* ctx, uint32_t absoluteIndex);
  const void* getterContext;
  std::size_t first;
  std::size_t count;
};

const char* getVisibleRowPrewarmText(const void* opaqueContext, const std::uint32_t index) {
  static constexpr char ellipsis[] = "\xe2\x80\xa6";
  const auto& context = *static_cast<const VisibleRowsPrewarmContext*>(opaqueContext);
  if (index >= context.count) return ellipsis;
  return context.getter(context.getterContext, static_cast<uint32_t>(context.first + index));
}

const char* getVectorRowText(const void* opaqueContext, const uint32_t index) {
  const auto& labels = *static_cast<const std::vector<std::string>*>(opaqueContext);
  return index < labels.size() ? labels[index].c_str() : "";
}
}  // namespace

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  invalidateListFontPrewarm();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::moveSelectionTo(const int index) {
  // No render lock: `selected` is written only here, on the main task, and the
  // viewport pull is deferred to the next build, where ListNav::syncToProps
  // consumes followOnBuild. Taking the lock parked the main loop for a whole
  // e-ink refresh, and the buttons are sampled once per loop pass from level
  // state with no queue, so a press that started and ended inside that window
  // was never seen at all.
  auto& n = activeNav();
  n.selected = index;
  n.followOnBuild = true;  // the next build pulls the viewport to it
  requestUpdate();
}

int UiListActivity::prewarmVisibleListRowsIfNeeded(const int fontId, const std::vector<std::string>& labels,
                                                   int first, int count) {
  return prewarmVisibleListRowsIfNeeded(fontId, &getVectorRowText, &labels, static_cast<int>(labels.size()), first,
                                        count);
}

int UiListActivity::prewarmVisibleListRowsIfNeeded(const int fontId, const ListRowTextGetter getter, const void* ctx,
                                                   const int rowCount, int first, int count) {
  if (!getter || rowCount <= 0) {
    invalidateListFontPrewarm();
    return 0;
  }
  if (!renderer.isSdCardFont(fontId)) {
    invalidateListFontPrewarm();
    return 0;
  }

  first = std::clamp(first, 0, rowCount);
  count = std::clamp(count, 0, rowCount - first);
  if (listFontPrewarmValid && lastPrewarmedFontId == fontId && lastPrewarmedFirst == first &&
      lastPrewarmedCount == count) {
    return 0;
  }

  const VisibleRowsPrewarmContext context{getter, ctx, static_cast<std::size_t>(first),
                                          static_cast<std::size_t>(count)};
  const int missed = DynamicFont::prewarmIfSdFont(renderer, fontId, &getVisibleRowPrewarmText, &context,
                                                  static_cast<uint32_t>(count + 1));
  lastPrewarmedFontId = fontId;
  lastPrewarmedFirst = first;
  lastPrewarmedCount = count;
  listFontPrewarmValid = true;
  return missed;
}

void UiListActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    // Keep viewport updates atomic with the render task's layout feedback.
    {
      RenderLock lock(*this);
      auto& n = activeNav();
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
      moved = n.scrollBy(delta, listCount());
    }
    if (moved) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, count, &n] { moveSelectionTo(ButtonNavigator::nextIndex(n.selected, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousIndex(n.selected, count)); });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown. The measurement
  // can be one build old while a refresh is in flight; the next layout's
  // feedback corrects the viewport.
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, count, n.pageRows())); });
  buttonNavigator.onPreviousContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, count, n.pageRows())); });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  props.partialTrailingRow = true;
  auto& n = activeNav();
  const int prevTop = n.top;
  const bool trusted = n.trusts(listCount());
  const int drawn = n.drawnRows;

  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default, so lists fit
    // as many rows per screen as they did before the FreeInkUI migration.
    // props.rowHeight must be set explicitly: screen.list() otherwise falls
    // back to the (touch-friendly) theme token, not this local value.
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
    // A label that must wrap (maxLines > 1) grows only its own row: list()
    // sizes wrapped items per-row, so the dense height stays for the rest.
  }
  n.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, listCount(), props);

  // When the selection is already visible in the current viewport (based on
  // the measured drawnRows rather than the unweighted visibleRows estimate),
  // keep selection-follow anchored instead of jumping to top. Explicit swipe
  // scrolling clears followPending and must retain its new viewport.
  if (n.followPending && trusted && drawn > 0) {
    const int sel = props.selectedIndex;
    if (sel >= prevTop && sel < prevTop + drawn) {
      n.top = prevTop;
      props.topIndex = static_cast<uint16_t>(prevTop);
    }
  }
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  // Wrapped labels can fit fewer rows than the fixed-height estimate
  // ListNav planned with. list() reports the real layout back
  // (ListNav::onListRendered); when the selection landed past the drawn rows,
  // the nav advances the viewport and asks for another build. The bound keeps
  // malformed/very tall content from monopolising a refresh.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  drawFooter();
  renderer.displayBuffer();
}
