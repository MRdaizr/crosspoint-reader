#pragma once

#include <Epub/Page.h>
#include <Memory.h>

#include "activities/Activity.h"

// All internal references, not only numbered notes. Source identity groups
// wrapped segments without merging separate anchors that share the same href.
class EpubReaderFootnoteSelectActivity final : public Activity {
  std::unique_ptr<Page> page;
  int marginLeft, marginTop;
  uint8_t choices[Page::MAX_LINKS_PER_PAGE] = {};
  int choiceCount = 0, selected = 0, snapshotChoice = -1;
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int snapshotX = 0, snapshotY = 0, snapshotW = 0, snapshotH = 0;
  GfxRenderer::Orientation snapshotOrientation = GfxRenderer::Portrait;

  void jump(int linkIndex);
  void drawHints() const;
  bool drawHighlight();
  void moveSelection(int dx, int dy);

 public:
  EpubReaderFootnoteSelectActivity(GfxRenderer& renderer, MappedInputManager& input, std::unique_ptr<Page> page,
                                   int left, int top)
      : Activity("EpubReaderFootnoteSelect", renderer, input),
        page(std::move(page)),
        marginLeft(left),
        marginTop(top) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleForcedRefresh() override {
    snapshotChoice = -1;
    requestUpdate();
    return true;
  }
};
