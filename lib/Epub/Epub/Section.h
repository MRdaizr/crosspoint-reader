#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "ReaderRenderSpec.h"

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
    uint32_t visibleTextOffset;
  };
  std::string buildFilePath;
  std::string buildHtmlPath;
  std::string buildIndexPath;
  HalFile buildFile;
  std::vector<BuildPageEntry> buildLut;
  std::unique_ptr<ChapterHtmlSlimParser> buildParser;
  CssParser* buildCssParser = nullptr;
  bool buildActive = false;
  // The streamed HTML may be retained across a layout-only rebuild.  Keep the
  // flag separate from buildActive so cleanup can remove a malformed fresh
  // stream while preserving a known-good cache for the next resume.
  bool buildHtmlReused_ = false;
  bool lowMemoryPauseLogged = false;
  uint16_t resumePageCount = 0;
  uint16_t builtPageCount = 0;

  // A committed partial cache is a readable page prefix.  Keep its watermark
  // separate from pages produced by the currently active parser so a rebuild
  // can extend an old prefix without exposing an incomplete trailing page.
  bool partial_ = false;
  uint16_t partialPageCount_ = 0;
  uint32_t partialBytesConsumed_ = 0;
  uint32_t partialTotalBytes_ = 0;

  void writeSectionFileHeader(HalFile& target, int fontId, float lineCompression, bool extraParagraphSpacing,
                              uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  uint32_t onIncrementalPageComplete(std::unique_ptr<Page> page, uint16_t paragraphIndex, uint16_t listItemIndex,
                                     uint32_t visibleTextOffset);
  bool finishIncrementalBuild();
  bool commitIncrementalBuild(uint8_t version, uint32_t bytesConsumed, uint32_t totalBytes);
  void discardIncrementalBuild(bool keepHtml = false);
  void preserveIncrementalBuild();
  std::unique_ptr<Page> loadPageAt(int page) const;
  bool resumeIncrementalBuild(int fontId, float lineCompression, bool extraParagraphSpacing,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                              bool focusReadingEnabled, const std::function<void(uint8_t)>& progressFn);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool loadSectionFile(const ReaderRenderSpec& spec) {
    return loadSectionFile(spec.fontId, spec.lineCompression, spec.extraParagraphSpacing, spec.paragraphAlignment,
                           spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled, spec.embeddedStyle,
                           spec.imageRendering, spec.focusReadingEnabled);
  }
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr,
                         const std::function<void(uint8_t)>& progressFn = nullptr);
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr,
                         const std::function<void(uint8_t)>& progressFn = nullptr) {
    return createSectionFile(spec.fontId, spec.lineCompression, spec.extraParagraphSpacing, spec.paragraphAlignment,
                             spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled, spec.embeddedStyle,
                             spec.imageRendering, spec.focusReadingEnabled, popupFn, progressFn);
  }
  bool beginIncrementalBuild(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                             uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                             bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled,
                             const std::function<void(uint8_t)>& progressFn = nullptr);
  bool beginIncrementalBuild(const ReaderRenderSpec& spec, const std::function<void(uint8_t)>& progressFn = nullptr) {
    return beginIncrementalBuild(spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
                                 spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight,
                                 spec.hyphenationEnabled, spec.embeddedStyle, spec.imageRendering,
                                 spec.focusReadingEnabled, progressFn);
  }
  enum class BuildResult { InProgress, Complete, PausedLowMemory, Failed };
  BuildResult buildNextChunk(uint8_t maxChunks = 1);
  bool isBuilding() const { return buildActive; }
  bool isPartial() const { return partial_; }
  uint16_t estimatedTotalPages() const;
  // Persist the pages already produced by an active parser as a readable
  // partial section.  This is used by the destructor and therefore covers
  // navigation, sleep and other activity teardown paths.
  void suspendBuild();
  // Drop an active build and any partial cache after an unrecoverable parse or
  // page-deserialization error.
  void abandonBuild();
  bool hasIncrementalBuildCheckpoint() const {
    return Storage.exists(buildFilePath.c_str()) && Storage.exists(buildHtmlPath.c_str()) &&
           Storage.exists(buildIndexPath.c_str());
  }
  bool hasBuiltPage(int page) const { return page >= 0 && page < pageCount; }
  std::unique_ptr<Page> buildPagePreview(int fontId, float lineCompression, bool extraParagraphSpacing,
                                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                                         bool focusReadingEnabled, uint16_t targetPage);
  // Public name used by the unified reader layer while an incremental build is
  // catching up.  If the requested page has already been serialized by the
  // active parser, read it directly from the .building file.  Falling back to
  // the committed partial or bounded preview keeps this API useful before the
  // first build tick as well, without changing section v41 on-disk semantics.
  std::unique_ptr<Page> loadPageDuringBuild(const ReaderRenderSpec& spec, uint16_t targetPage);
  std::unique_ptr<Page> loadPageFromSectionFile();
  // Read a page through the active incremental build when available, falling
  // back to the committed (final or partial) section file.  Keeping this
  // lookup independent from currentPage lets bookmark/KOReader resolution
  // inspect a target page while the parser is still catching up.
  std::unique_ptr<Page> loadPage(int page);
  std::string getTextFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Resolve an anchor from the parser's in-memory map.  Only pages that have
  // already been serialized are returned; an anchor may be observed before
  // its containing page is flushed, in which case the caller should keep the
  // target pending and let the next build slice advance.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;
  std::optional<uint16_t> findAnchor(const std::string& anchor) const;

  // True when the streamed chapter HTML is available in the section cache.
  // Reusing it avoids inflating the same ZIP entry again during a resumed
  // incremental build.  The file is still validated by the parser.
  bool hasHtmlCache() const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Exact zero-based visible Unicode-codepoint offset for a rendered page.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;

  // Resolve an exact visible-text offset to the page containing it. When
  // preferFirstAtOffset is true, ties caused by zero-width content (for
  // example an image-only page) select the first page at that offset.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;

  // True when the active parser has emitted a page at or beyond a visible
  // offset.  This is the content-based counterpart of hasBuiltPage() and is
  // used to decide whether a KOReader/bookmark jump can be applied now or
  // must wait for another parser slice.
  bool buildReachedVisibleTextOffset(uint32_t offset) const {
    return buildActive && !buildLut.empty() && offset <= buildLut.back().visibleTextOffset;
  }
};
