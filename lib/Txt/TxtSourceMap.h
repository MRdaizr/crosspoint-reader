#pragma once
#include <string>

#include "TxtToHtml.h"

// SD-backed sparse index; every lookup replays the identical conversion machine.
// Offsets within a UTF8 codepoint map to its start. Dropped text maps forward to
// the next emitted character. The synthetic prefix/footer map to source 0/EOF.
class TxtSourceMap {
  std::string sourcePath, cachePath, htmlPath, mapPath;
  uint32_t sourceSize = 0, visibleCount = 0, htmlSize = 0, recordCount = 0;
  uint64_t sourceHash = 0;
  bool ready = false;
  bool sourceChanged = false;
  bool lookup(uint32_t target, bool reverse, uint32_t& result, const TxtToHtml::Callbacks* callbacks) const;

 public:
  TxtSourceMap(std::string sourcePath, std::string cachePath)
      : sourcePath(std::move(sourcePath)), cachePath(std::move(cachePath)) {}
  bool prepare(bool buildIfMissing = true, const TxtToHtml::Callbacks* callbacks = nullptr);
  bool sourceToVisible(uint32_t sourceOffset, uint32_t& visibleOffset,
                       const TxtToHtml::Callbacks* callbacks = nullptr) const;
  bool visibleToSource(uint32_t visibleOffset, uint32_t& sourceOffset,
                       const TxtToHtml::Callbacks* callbacks = nullptr) const;
  bool isSourceBoundary(uint32_t sourceOffset) const;
  bool isReady() const { return ready; }
  bool wasSourceChanged() const { return sourceChanged; }
  uint32_t getSourceSize() const { return sourceSize; }
  uint32_t getVisibleCount() const { return visibleCount; }
  uint32_t getHtmlSize() const { return htmlSize; }
  uint64_t getSourceHash() const { return sourceHash; }
  const std::string& getHtmlPath() const { return htmlPath; }
};
