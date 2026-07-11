#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  struct BuildPageEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
  };
  std::string buildFilePath;
  std::string buildHtmlPath;
  HalFile buildFile;
  std::vector<BuildPageEntry> buildLut;
  std::unique_ptr<ChapterHtmlSlimParser> buildParser;
  CssParser* buildCssParser = nullptr;
  bool buildActive = false;
  bool lowMemoryPauseLogged = false;

  void writeSectionFileHeader(HalFile& target, int fontId, float lineCompression, bool extraParagraphSpacing,
                              uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  uint32_t onIncrementalPageComplete(std::unique_ptr<Page> page, uint16_t paragraphIndex, uint16_t listItemIndex);
  bool finishIncrementalBuild();
  void discardIncrementalBuild();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr,
                         const std::function<void(uint8_t)>& progressFn = nullptr);
  bool beginIncrementalBuild(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                             uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                             bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled,
                             const std::function<void(uint8_t)>& progressFn = nullptr);
  enum class BuildResult { InProgress, Complete, PausedLowMemory, Failed };
  BuildResult buildNextChunk(uint8_t maxChunks = 1);
  bool isBuilding() const { return buildActive; }
  bool hasBuiltPage(int page) const { return page >= 0 && page < pageCount; }
  std::unique_ptr<Page> buildPagePreview(int fontId, float lineCompression, bool extraParagraphSpacing,
                                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                                         bool focusReadingEnabled, uint16_t targetPage);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
