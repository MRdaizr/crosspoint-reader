#include "EpubReaderFootnoteSelectActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

void EpubReaderFootnoteSelectActivity::onEnter() {
  Activity::onEnter();
  if (page) {
    for (size_t i = 0; i < page->links.size(); ++i) {
      if (page->links.firstOccurrence(i)) choices[choiceCount++] = static_cast<uint8_t>(i);
    }
  }
  // One bounded reusable allocation, never a second framebuffer. OOM simply
  // disables differential repaint; the owned page can always be rendered again.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  if (!snapshot) LOG_ERR("LNK", "OOM: highlight snapshot; full redraw");
  requestUpdate();
}

void EpubReaderFootnoteSelectActivity::onExit() {
  snapshot.reset();
  page.reset();
  Activity::onExit();
}

void EpubReaderFootnoteSelectActivity::jump(const int index) {
  if (!page || index < 0 || index >= static_cast<int>(page->links.size())) return;
  ActivityResult result;
  result.data = FootnoteResult{page->links[index].href.get()};
  setResult(std::move(result));
  finish();
}

void EpubReaderFootnoteSelectActivity::drawHints() const {
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                       tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderFootnoteSelectActivity::moveSelection(const int dx, const int dy) {
  if (choiceCount == 0) return;
  const auto& current = page->links[choices[selected]];
  const int cx = current.x * 2 + current.width, cy = current.y * 2 + current.height;
  int best = selected, bestScore = INT32_MAX;
  for (int i = 0; i < choiceCount; ++i) {
    const auto& candidate = page->links[choices[i]];
    const int x = candidate.x * 2 + candidate.width - cx;
    const int y = candidate.y * 2 + candidate.height - cy;
    const int forward = dx ? x * dx : y * dy;
    if (forward <= 0) continue;
    const int score = forward + 4 * std::abs(dx ? y : x);
    if (score < bestScore) {
      best = i;
      bestScore = score;
    }
  }
  if (best == selected) {
    // Reading-order fallback for the first/last link in a row (also RTL).
    best = std::clamp(selected + (dx + dy > 0 ? 1 : -1), 0, choiceCount - 1);
  }
  if (best != selected) {
    selected = best;
    requestUpdate();
  }
}

void EpubReaderFootnoteSelectActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  int x = 0, y = 0;
  if (page && mappedInput.wasScreenTapped(x, y)) {
    const int hit = page->links.hitTest(x - marginLeft, y - marginTop);
    if (hit >= 0) {
      jump(hit);
      return;
    }
  }
  if (!choiceCount) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    jump(choices[selected]);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft))
    moveSelection(-1, 0);
  else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenRight))
    moveSelection(1, 0);
  else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp))
    moveSelection(0, -1);
  else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown))
    moveSelection(0, 1);
}

bool EpubReaderFootnoteSelectActivity::drawHighlight() {
  snapshotChoice = -1;
  if (!page || !choiceCount) return false;
  const uint32_t identity = page->links[choices[selected]].identity;
  int x1 = renderer.getScreenWidth(), y1 = renderer.getScreenHeight(), x2 = 0, y2 = 0;
  for (const auto& link : page->links)
    if (link.identity == identity) {
      x1 = std::min(x1, link.x + marginLeft - 2);
      y1 = std::min(y1, link.y + marginTop - 2);
      x2 = std::max(x2, link.x + marginLeft + link.width + 2);
      y2 = std::max(y2, link.y + marginTop + link.height + 2);
    }
  snapshotX = std::max(0, x1);
  snapshotY = std::max(0, y1);
  snapshotW = std::min(renderer.getScreenWidth(), x2) - snapshotX;
  snapshotH = std::min(renderer.getScreenHeight(), y2) - snapshotY;
  bool saved = false;
  if (snapshot && snapshotW > 0 && snapshotH > 0) {
    const size_t bytes = renderer.getRegionByteSize(snapshotX, snapshotY, snapshotW, snapshotH);
    saved = bytes > 0 && bytes <= SNAPSHOT_CAPACITY &&
            renderer.copyRegionToBuffer(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get(), SNAPSHOT_CAPACITY);
  }
  if (saved) {
    snapshotChoice = selected;
    snapshotOrientation = renderer.getOrientation();
  }
  for (const auto& link : page->links)
    if (link.identity == identity) {
      renderer.drawRect(link.x + marginLeft - 2, link.y + marginTop - 2, link.width + 4, link.height + 4, 2, true);
    }
  return saved;
}

void EpubReaderFootnoteSelectActivity::render(RenderLock&&) {
  const bool restored =
      snapshotChoice >= 0 && snapshotOrientation == renderer.getOrientation() && snapshot &&
      renderer.copyBufferToRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get(), SNAPSHOT_CAPACITY);
  if (!restored) {
    renderer.clearScreen();
    if (page) page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
  }
  // A failed capture is safe even after restore: the next frame fully redraws.
  if (!drawHighlight() && restored) {
    renderer.clearScreen();
    if (page) page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
    drawHighlight();
  }
  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
