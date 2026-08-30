#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: adds a visible-text offset LUT for exact cross-reader resumes.
// v29: expands footnote href storage and invalidates updated CJK token spacing
//      and continuation semantics.
// v30: stores each TextBlock in a single arena and changes its line payload.
// v31: TokenBoundary changes Focus Reading continuation semantics: explicit
// visible hyphens are legal wrap points. Invalidate old layout caches so
// their line/page indexes are rebuilt with the new boundaries.
// v32: ImageBlock stores the book-internal source href for lazy image
// extraction. Existing pages only stored the extracted cache path, so old
// section files cannot be safely reinterpreted and must be rebuilt.
// v33: simple HTML table rows are laid out as positioned columns instead of
//      flattened paragraphs with synthetic row/cell labels.
// v34: TextBlock stores bounded per-token Ruby annotations and reserves
//      additional line height for <ruby>/<rt> rendering.
// v35: Ruby follower tokens are explicitly marked as no-break boundaries so
//      grouped annotations cannot be split across pages.
// v36: Ruby/CJK justification changes invalidate cached word positions.
// v37: Footnote href records grew from 96 to 256 bytes.
// v38: Focus Reading hyphen/dash breaks and whole-word hyphenation changed.
// v39: Image top-margin placement is clamped at the viewport boundary.
// v40: Ruby groups survive a soft flush of a large text block.
// v41: Simple HTML table rows are laid out as positioned columns.
constexpr uint8_t SECTION_FILE_VERSION = 41;
// A section being written is never readable. The version is stamped with the
// final/partial value only after all page tables have been written.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Derived from the format version so a partial made by a different layout
// cannot be mistaken for this one. v28 -> 0xFE, v41 -> 0xF1.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
constexpr size_t MIN_INCREMENTAL_FREE_HEAP = 48 * 1024;
constexpr size_t MIN_INCREMENTAL_MAX_ALLOC = 32 * 1024;
constexpr uint16_t INCREMENTAL_PARSE_BUFFER_SIZE = 256;
constexpr uint32_t BUILD_CHECKPOINT_MAGIC = 0x43504231;  // CPB1
// ImageBlock page records gained a serialized source href in section v32. An
// old .building file must not be resumed and have new-format pages appended to
// its old-format prefix, so invalidate the incremental checkpoint as well.
// v4 also invalidates partially built sections after the table column layout
// change, which otherwise could mix old flattened rows with new grid rows.
// v5 invalidates any checkpoint whose pages lack Ruby payloads; v6 aligns the
// checkpoint with section semantics v36-v41 and the partial-cache format.
constexpr uint16_t BUILD_CHECKPOINT_VERSION = 6;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t visibleTextOffset;
};

struct BuildCheckpointHeader {
  uint32_t magic;
  uint16_t version;
  uint32_t layoutHash;
};

uint32_t buildLayoutHash(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                         const uint8_t paragraphAlignment, const uint16_t viewportWidth, const uint16_t viewportHeight,
                         const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                         const bool focusReadingEnabled) {
  uint32_t hash = 2166136261UL;
  const auto mix = [&hash](const uint32_t value) { hash = (hash ^ value) * 16777619UL; };
  uint32_t compressionBits = 0;
  static_assert(sizeof(compressionBits) == sizeof(lineCompression));
  std::memcpy(&compressionBits, &lineCompression, sizeof(compressionBits));
  mix(static_cast<uint32_t>(fontId));
  mix(compressionBits);
  mix(extraParagraphSpacing);
  mix(paragraphAlignment);
  mix(viewportWidth);
  mix(viewportHeight);
  mix(hyphenationEnabled);
  mix(embeddedStyle);
  mix(imageRendering);
  mix(focusReadingEnabled);
  return hash;
}
}  // namespace

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin"),
      buildFilePath(filePath + ".building"),
      buildHtmlPath(epub->getCachePath() + "/.tmp_build_" + std::to_string(spineIndex) + ".html"),
      buildIndexPath(buildFilePath + ".idx") {}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", builtPageCount);

  builtPageCount++;
  pageCount = std::max(pageCount, builtPageCount);
  return position;
}

Section::~Section() { suspendBuild(); }

uint32_t Section::onIncrementalPageComplete(std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                                             const uint16_t listItemIndex, const uint32_t visibleTextOffset) {
  if (!buildFile) {
    LOG_ERR("SCT", "Build file not open for page %d", builtPageCount);
    return 0;
  }

  const uint32_t position = buildFile.position();
  if (!page->serialize(buildFile)) {
    LOG_ERR("SCT", "Failed to serialize incremental page %d", builtPageCount);
    return 0;
  }
  buildFile.flush();
  buildLut.push_back({position, paragraphIndex, listItemIndex, visibleTextOffset});
  HalFile checkpoint;
  checkpoint = Storage.open(buildIndexPath.c_str(), O_RDWR | O_AT_END);
  if (!checkpoint) {
    LOG_ERR("SCT", "Failed to append incremental build checkpoint");
  } else {
    const BuildPageEntry& entry = buildLut.back();
    checkpoint.write(&entry, sizeof(entry));
    checkpoint.flush();
    checkpoint.close();
  }
  builtPageCount++;
  pageCount = std::max(pageCount, builtPageCount);
  return position;
}

void Section::preserveIncrementalBuild() {
  buildParser.reset();
  if (buildFile) {
    buildFile.close();
  }
  if (buildCssParser) {
    buildCssParser->clear();
    buildCssParser = nullptr;
  }
  buildLut.clear();
  buildActive = false;
  buildHtmlReused_ = false;
  builtPageCount = 0;
  resumePageCount = 0;
}

void Section::discardIncrementalBuild(const bool keepHtml) {
  preserveIncrementalBuild();
  if (!keepHtml && !buildHtmlPath.empty() && Storage.exists(buildHtmlPath.c_str())) {
    Storage.remove(buildHtmlPath.c_str());
  }
  if (Storage.exists(buildFilePath.c_str())) {
    Storage.remove(buildFilePath.c_str());
  }
  if (Storage.exists(buildIndexPath.c_str())) {
    Storage.remove(buildIndexPath.c_str());
  }
  resumePageCount = 0;
  builtPageCount = 0;
  buildHtmlReused_ = keepHtml;
}

bool Section::commitIncrementalBuild(const uint8_t version, const uint32_t bytesConsumed,
                                     const uint32_t totalBytes) {
  if (!buildFile || !buildParser) {
    LOG_ERR("SCT", "Cannot commit incremental section cache without active parser");
    return false;
  }

  const bool asPartial = version == SECTION_FILE_PARTIAL_VERSION;
  const auto failCommit = [this]() {
    buildFile.close();
    Storage.remove(buildFilePath.c_str());
    return false;
  };

  const uint32_t lutOffset = buildFile.position();
  for (const auto& entry : buildLut) {
    if (entry.fileOffset < HEADER_SIZE) {
      LOG_ERR("SCT", "Failed to write incremental page LUT");
      return failCommit();
    }
    serialization::writePod(buildFile, entry.fileOffset);
  }

  const uint32_t anchorMapOffset = buildFile.position();
  const auto& anchors = buildParser->getAnchors();
  uint16_t anchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    if (!asPartial || page < builtPageCount) anchorCount++;
  }
  serialization::writePod(buildFile, anchorCount);
  for (const auto& [anchor, page] : anchors) {
    if (asPartial && page >= builtPageCount) continue;
    serialization::writeString(buildFile, anchor);
    serialization::writePod(buildFile, page);
  }

  const uint32_t paragraphLutOffset = buildFile.position();
  serialization::writePod(buildFile, static_cast<uint16_t>(buildLut.size()));
  for (const auto& entry : buildLut) {
    serialization::writePod(buildFile, entry.paragraphIndex);
  }

  const uint32_t liLutOffset = buildFile.position();
  for (const auto& entry : buildLut) {
    serialization::writePod(buildFile, entry.listItemIndex);
  }

  const uint32_t visibleLutOffset = buildFile.position();
  for (const auto& entry : buildLut) {
    serialization::writePod(buildFile, entry.visibleTextOffset);
  }

  if (asPartial) {
    serialization::writePod(buildFile, bytesConsumed);
    serialization::writePod(buildFile, totalBytes);
  }

  // Patch all offsets first, then stamp the version byte last. A reset or power
  // loss before the stamp leaves version 0, which loadSectionFile rejects.
  buildFile.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(pageCount));
  serialization::writePod(buildFile, builtPageCount);
  serialization::writePod(buildFile, lutOffset);
  serialization::writePod(buildFile, anchorMapOffset);
  serialization::writePod(buildFile, paragraphLutOffset);
  serialization::writePod(buildFile, liLutOffset);
  serialization::writePod(buildFile, visibleLutOffset);
  buildFile.seek(0);
  serialization::writePod(buildFile, version);
  buildFile.flush();
  buildFile.close();

  // Keep the same atomic-ish swap used by the existing X4 builder. The old
  // partial is only replaced after all page tables and the trailer are valid.
  if (Storage.exists(filePath.c_str()) && !Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to replace old section cache");
    Storage.remove(buildFilePath.c_str());
    return false;
  }
  if (!Storage.rename(buildFilePath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to commit incremental section cache");
    Storage.remove(buildFilePath.c_str());
    return false;
  }
  return true;
}

bool Section::finishIncrementalBuild() {
  if (!buildFile || !buildParser) return false;
  const bool committed = commitIncrementalBuild(SECTION_FILE_VERSION, 0, 0);
  if (buildCssParser) {
    buildCssParser->clear();
    buildCssParser = nullptr;
  }
  buildParser.reset();
  buildLut.clear();
  buildActive = false;
  buildHtmlReused_ = false;
  resumePageCount = 0;
  if (!committed) return false;
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  pageCount = builtPageCount;
  builtPageCount = 0;
  return true;
}

void Section::writeSectionFileHeader(HalFile& target, const int fontId, const float lineCompression,
                                     const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool focusReadingEnabled) {
  if (!target) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(focusReadingEnabled) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(target, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(target, fontId);
  serialization::writePod(target, lineCompression);
  serialization::writePod(target, extraParagraphSpacing);
  serialization::writePod(target, paragraphAlignment);
  serialization::writePod(target, viewportWidth);
  serialization::writePod(target, viewportHeight);
  serialization::writePod(target, hyphenationEnabled);
  serialization::writePod(target, embeddedStyle);
  serialization::writePod(target, imageRendering);
  serialization::writePod(target, focusReadingEnabled);
  serialization::writePod(target, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(target, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(target, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(target, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(target, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
  serialization::writePod(target, static_cast<uint32_t>(0));  // Placeholder for visible offset LUT (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled) {
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  pageCount = 0;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  if (file.size() < HEADER_SIZE) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: truncated section header");
    clearCache();
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = version == SECTION_FILE_PARTIAL_VERSION;
    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  if (filePartial && pageCount == 0) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: section has no pages");
    clearCache();
    return false;
  }

  // Partial files carry the same LUT layout as a finalized section followed by
  // a two-word parse watermark. Validate the offsets before accepting the
  // prefix so a torn SD write cannot expose arbitrary page offsets.
  if (filePartial) {
    uint32_t liLutOffset = 0;
    uint32_t visibleLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
    serialization::readPod(file, liLutOffset);
    file.seek(HEADER_SIZE - sizeof(uint32_t));
    serialization::readPod(file, visibleLutOffset);
    const uint32_t trailerOffset = visibleLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
    const bool valid = liLutOffset >= HEADER_SIZE && visibleLutOffset > liLutOffset &&
                       trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!valid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    file.seek(trailerOffset);
    serialization::readPod(file, partialBytesConsumed_);
    serialization::readPod(file, partialTotalBytes_);
    partial_ = true;
    partialPageCount_ = pageCount;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, partial_ ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  bool success = true;
  const auto removeIfPresent = [&success](const std::string& path) {
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) success = false;
  };
  removeIfPresent(filePath);
  removeIfPresent(buildFilePath);
  removeIfPresent(buildIndexPath);
  removeIfPresent(buildHtmlPath);
  if (!success) {
    LOG_ERR("SCT", "Failed to clear section cache artifacts");
    return false;
  }
  LOG_DBG("SCT", "Section cache cleared");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const std::function<void()>& popupFn, const std::function<void(uint8_t)>& progressFn) {
  pageCount = 0;
  builtPageCount = 0;
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(file, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cacheResult = cssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to load CSS cache; section build deferred");
        cssParser->clear();
        file.close();
        Storage.remove(filePath.c_str());
        Storage.remove(tmpHtmlPath.c_str());
        return false;
      }
      if (cacheResult != CssParser::CacheLoadResult::Complete) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                   const uint32_t visibleTextOffset) {
        lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex, visibleTextOffset});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, 0,
      progressFn);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  const uint32_t visibleLutOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.visibleTextOffset);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  serialization::writePod(file, visibleLutOffset);
  file.seek(0);
  serialization::writePod(file, SECTION_FILE_VERSION);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  builtPageCount = 0;
  return true;
}

bool Section::resumeIncrementalBuild(
    const int fontId, const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
    const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
    const bool embeddedStyle, const uint8_t imageRendering, const bool focusReadingEnabled,
    const std::function<void(uint8_t)>& progressFn) {
  if (!Storage.exists(buildFilePath.c_str()) || !Storage.exists(buildHtmlPath.c_str()) ||
      !Storage.exists(buildIndexPath.c_str())) {
    return false;
  }

  HalFile checkpoint;
  if (!Storage.openFileForRead("SCT", buildIndexPath, checkpoint)) {
    return false;
  }
  BuildCheckpointHeader header{};
  const uint32_t layoutHash =
      buildLayoutHash(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                      viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  if (checkpoint.read(&header, sizeof(header)) != sizeof(header) || checkpoint.size() < sizeof(header) ||
      header.magic != BUILD_CHECKPOINT_MAGIC ||
      header.version != BUILD_CHECKPOINT_VERSION || header.layoutHash != layoutHash ||
      (checkpoint.size() - sizeof(header)) % sizeof(BuildPageEntry) != 0) {
    checkpoint.close();
    return false;
  }

  HalFile partialBuild;
  if (!Storage.openFileForRead("SCT", buildFilePath, partialBuild)) {
    checkpoint.close();
    return false;
  }
  uint8_t buildVersion = SECTION_FILE_INCOMPLETE_VERSION;
  partialBuild.seek(0);
  serialization::readPod(partialBuild, buildVersion);
  if (buildVersion != SECTION_FILE_INCOMPLETE_VERSION) {
    LOG_INF("SCT", "Discarding incremental checkpoint with committed version %u", buildVersion);
    partialBuild.close();
    checkpoint.close();
    return false;
  }
  const size_t partialBuildSize = partialBuild.fileSize();
  partialBuild.close();

  buildLut.clear();
  BuildPageEntry entry{};
  while (checkpoint.read(&entry, sizeof(entry)) == sizeof(entry)) {
    // A page entry is written only after its serialized page is flushed.  Still
    // validate it here: power loss can leave the checkpoint one record ahead
    // of the data file on some SD cards.
    if (entry.fileOffset < HEADER_SIZE || entry.fileOffset >= partialBuildSize) {
      LOG_INF("SCT", "Discarding incomplete incremental checkpoint");
      checkpoint.close();
      buildLut.clear();
      return false;
    }
    buildLut.push_back(entry);
  }
  checkpoint.close();
  if (buildLut.empty()) {
    return false;
  }

  buildFile = Storage.open(buildFilePath.c_str(), O_RDWR | O_AT_END);
  if (!buildFile) {
    return false;
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const size_t lastSlash = localPath.find_last_of('/');
  const std::string contentBase = lastSlash != std::string::npos ? localPath.substr(0, lastSlash + 1) : "";
  const std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";
  if (embeddedStyle) {
    buildCssParser = epub->getCssParser();
    if (buildCssParser) {
      const auto cacheResult = buildCssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to load CSS cache for resumed build; retry later");
        buildCssParser->clear();
        // The open .building file and checkpoint must not survive this retry
        // path: otherwise the next resume can keep replaying a partial build.
        discardIncrementalBuild();
        return false;
      }
      if (cacheResult != CssParser::CacheLoadResult::Complete) {
        LOG_ERR("SCT", "Failed to load CSS from cache for resumed build");
      }
    }
  }

  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto tocEntry = epub->getTocItem(i);
      if (tocEntry.spineIndex != spineIndex) break;
      if (!tocEntry.anchor.empty()) tocAnchors.push_back(std::move(tocEntry.anchor));
    }
  }

  builtPageCount = static_cast<uint16_t>(buildLut.size());
  pageCount = std::max(partial_ ? partialPageCount_ : static_cast<uint16_t>(0), builtPageCount);
  resumePageCount = builtPageCount;
  buildActive = true;
  buildHtmlReused_ = true;
  buildParser = std::make_unique<ChapterHtmlSlimParser>(
      epub, buildHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
             const uint32_t visibleTextOffset) {
        if (resumePageCount > 0) {
          resumePageCount--;
          return;
        }
        onIncrementalPageComplete(std::move(page), paragraphIndex, listItemIndex, visibleTextOffset);
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), nullptr, buildCssParser, 0,
      progressFn, INCREMENTAL_PARSE_BUFFER_SIZE);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  if (!buildParser->beginParsing()) {
    preserveIncrementalBuild();
    return false;
  }
  LOG_INF("SCT", "Resuming incremental build at page %d", pageCount);
  return true;
}

bool Section::beginIncrementalBuild(
    const int fontId, const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
    const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
    const bool embeddedStyle, const uint8_t imageRendering, const bool focusReadingEnabled,
    const std::function<void(uint8_t)>& progressFn) {
  if (resumeIncrementalBuild(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                             viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled,
                             progressFn)) {
    return true;
  }
  const bool reuseHtml = hasHtmlCache();
  discardIncrementalBuild(reuseHtml);
  // Keep a previously committed partial prefix available while this fresh
  // parser catches up from the beginning of the chapter.
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount = 0;

  const auto sectionsDir = epub->getCachePath() + "/sections";
  Storage.mkdir(sectionsDir.c_str());
  Storage.remove(buildFilePath.c_str());

  const auto localPath = epub->getSpineItem(spineIndex).href;
  // A completed/partial section may leave the streamed HTML behind. Reusing
  // it avoids inflating a large ZIP entry again when only layout settings
  // changed. The parser still validates the content before producing pages.
  bool streamed = reuseHtml;
  buildHtmlReused_ = reuseHtml;
  if (!streamed) {
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", buildHtmlPath, tmpHtml)) {
        continue;
      }
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
      tmpHtml.close();
      if (!streamed) {
        Storage.remove(buildHtmlPath.c_str());
        delay(50);
      }
    }
  }
  if (!streamed || !Storage.openFileForWrite("SCT", buildFilePath, buildFile)) {
    discardIncrementalBuild();
    return false;
  }

  writeSectionFileHeader(buildFile, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  HalFile checkpoint;
  if (!Storage.openFileForWrite("SCT", buildIndexPath, checkpoint)) {
    discardIncrementalBuild();
    return false;
  }
  const BuildCheckpointHeader checkpointHeader{
      BUILD_CHECKPOINT_MAGIC, BUILD_CHECKPOINT_VERSION,
      buildLayoutHash(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                      viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled)};
  checkpoint.write(&checkpointHeader, sizeof(checkpointHeader));
  checkpoint.close();

  size_t lastSlash = localPath.find_last_of('/');
  const std::string contentBase = lastSlash != std::string::npos ? localPath.substr(0, lastSlash + 1) : "";
  const std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (embeddedStyle) {
    buildCssParser = epub->getCssParser();
    if (buildCssParser) {
      const auto cacheResult = buildCssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to load CSS cache for incremental build; retry later");
        buildCssParser->clear();
        discardIncrementalBuild();
        return false;
      }
      if (cacheResult != CssParser::CacheLoadResult::Complete) {
        LOG_ERR("SCT", "Failed to load CSS from cache for incremental build");
      }
    }
  }

  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) tocAnchors.push_back(std::move(entry.anchor));
    }
  }

  buildActive = true;
  resumePageCount = 0;
  buildParser = std::make_unique<ChapterHtmlSlimParser>(
      epub, buildHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
             const uint32_t visibleTextOffset) {
        onIncrementalPageComplete(std::move(page), paragraphIndex, listItemIndex, visibleTextOffset);
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), nullptr, buildCssParser, 0,
      progressFn, INCREMENTAL_PARSE_BUFFER_SIZE);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  if (!buildParser->beginParsing()) {
    discardIncrementalBuild();
    return false;
  }
  return true;
}

Section::BuildResult Section::buildNextChunk(const uint8_t maxChunks) {
  if (!buildActive || !buildParser) {
    return BuildResult::Failed;
  }

  size_t freeHeap = esp_get_free_heap_size();
  size_t maxAlloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < MIN_INCREMENTAL_FREE_HEAP || maxAlloc < MIN_INCREMENTAL_MAX_ALLOC) {
    // The text prewarm cache is disposable. Releasing it here is substantially
    // cheaper than abandoning the chapter build, and it avoids holding a large
    // fragmented glyph cache while the parser needs contiguous working memory.
    if (auto* fontCache = renderer.getFontCacheManager()) {
      fontCache->clearCache();
      freeHeap = esp_get_free_heap_size();
      maxAlloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
  }
  if (freeHeap < MIN_INCREMENTAL_FREE_HEAP || maxAlloc < MIN_INCREMENTAL_MAX_ALLOC) {
    if (!lowMemoryPauseLogged) {
      LOG_INF("SCT", "Pausing incremental build for low memory: free=%u maxAlloc=%u", freeHeap, maxAlloc);
      lowMemoryPauseLogged = true;
    }
    return BuildResult::PausedLowMemory;
  }
  lowMemoryPauseLogged = false;

  const auto result = buildParser->parseNextChunk(maxChunks);
  if (result == ChapterHtmlSlimParser::ParseResult::InProgress) {
    return BuildResult::InProgress;
  }
  if (result == ChapterHtmlSlimParser::ParseResult::Failed || !finishIncrementalBuild()) {
    discardIncrementalBuild();
    return BuildResult::Failed;
  }
  return BuildResult::Complete;
}

uint16_t Section::estimatedTotalPages() const {
  const auto clampEstimate = [](const uint64_t value, const uint16_t minimum) {
    if (value <= minimum) return minimum;
    return static_cast<uint16_t>(std::min<uint64_t>(value, 60000));
  };

  uint16_t available = pageCount;
  uint32_t consumed = partialBytesConsumed_;
  uint32_t total = partialTotalBytes_;
  if (buildActive && buildParser) {
    consumed = static_cast<uint32_t>(buildParser->parseBytesConsumed());
    total = static_cast<uint32_t>(buildParser->parseTotalBytes());
    available = std::max(available, builtPageCount);
  }
  if (consumed == 0 || total <= consumed || available == 0) return available;
  return clampEstimate(static_cast<uint64_t>(available) * total / consumed, available);
}

void Section::suspendBuild() {
  if (!buildActive) return;

  // Persist only complete pages and only when this pass made progress beyond
  // an older partial. The current parser may still own an unfinished page.
  const bool worthKeeping = buildParser && buildFile && builtPageCount > 0 &&
                            (!partial_ || builtPageCount > partialPageCount_);
  bool committed = false;
  if (worthKeeping) {
    const uint32_t consumed = static_cast<uint32_t>(buildParser->parseBytesConsumed());
    const uint32_t total = static_cast<uint32_t>(buildParser->parseTotalBytes());
    committed = commitIncrementalBuild(SECTION_FILE_PARTIAL_VERSION, consumed, total);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = total;
      LOG_INF("SCT", "Suspended section build: %u pages persisted", builtPageCount);
    }
  }

  buildParser.reset();
  if (buildCssParser) {
    buildCssParser->clear();
    buildCssParser = nullptr;
  }
  if (buildFile) buildFile.close();
  if (!committed) Storage.remove(buildFilePath.c_str());
  Storage.remove(buildIndexPath.c_str());
  // Keep a known-good streamed HTML cache for the next layout-only rebuild.
  // Fresh streams (buildHtmlReused_ == false) are still discarded on suspend
  // to preserve the old cache footprint.
  if (!buildHtmlReused_) Storage.remove(buildHtmlPath.c_str());
  buildLut.clear();
  buildActive = false;
  buildHtmlReused_ = false;
  resumePageCount = 0;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount = 0;
}

void Section::abandonBuild() {
  if (buildParser) buildParser.reset();
  if (buildCssParser) {
    buildCssParser->clear();
    buildCssParser = nullptr;
  }
  if (buildFile) buildFile.close();
  Storage.remove(buildFilePath.c_str());
  Storage.remove(buildIndexPath.c_str());
  Storage.remove(buildHtmlPath.c_str());
  // A parse failure is deterministic for this HTML; retaining a partial would
  // make every subsequent open replay the same failing build.
  Storage.remove(filePath.c_str());
  buildLut.clear();
  buildActive = false;
  resumePageCount = 0;
  builtPageCount = 0;
  pageCount = 0;
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
}

std::unique_ptr<Page> Section::buildPagePreview(const int fontId, const float lineCompression,
                                                const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                                const bool hyphenationEnabled, const bool embeddedStyle,
                                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                                const uint16_t targetPage) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_preview_" + std::to_string(spineIndex) + ".html";

  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      delay(50);
    }
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    tmpHtml.close();
  }

  if (!success) {
    Storage.remove(tmpHtmlPath.c_str());
    return nullptr;
  }

  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cacheResult = cssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to load CSS cache for preview; retry later");
        cssParser->clear();
        Storage.remove(tmpHtmlPath.c_str());
        return nullptr;
      }
      if (cacheResult != CssParser::CacheLoadResult::Complete) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  std::unique_ptr<Page> previewPage;
  uint16_t builtPage = 0;
  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [&previewPage, &builtPage, targetPage](std::unique_ptr<Page> page, const uint16_t, const uint16_t,
                                             const uint32_t) {
        if (builtPage == targetPage) {
          previewPage = std::move(page);
        }
        builtPage++;
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, {}, nullptr, cssParser,
      static_cast<uint16_t>(targetPage + 1));
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (cssParser) {
    cssParser->clear();
  }

  pageCount = std::max<uint16_t>(static_cast<uint16_t>(targetPage + 1), builtPage);
  return success ? std::move(previewPage) : nullptr;
}

std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  if (page < 0) return nullptr;
  HalFile cached;
  if (!Storage.openFileForRead("SCT", filePath, cached) || cached.size() < HEADER_SIZE) {
    return nullptr;
  }

  uint8_t version = 0;
  serialization::readPod(cached, version);
  if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
    cached.close();
    return nullptr;
  }

  cached.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t cachedPageCount = 0;
  serialization::readPod(cached, cachedPageCount);
  if (page >= cachedPageCount) {
    cached.close();
    return nullptr;
  }

  cached.seek(HEADER_SIZE - sizeof(uint32_t) * 5);
  uint32_t lutOffset = 0;
  serialization::readPod(cached, lutOffset);
  const uint32_t lutEntry = lutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t);
  if (lutOffset < HEADER_SIZE || lutEntry + sizeof(uint32_t) > cached.size()) {
    cached.close();
    return nullptr;
  }
  cached.seek(lutEntry);
  uint32_t pagePos = 0;
  serialization::readPod(cached, pagePos);
  if (pagePos < HEADER_SIZE || pagePos >= cached.size()) {
    cached.close();
    return nullptr;
  }
  cached.seek(pagePos);
  auto result = Page::deserialize(cached);

  if (result) {
    cached.seek(HEADER_SIZE - sizeof(uint32_t));
    uint32_t visibleLutOffset = 0;
    serialization::readPod(cached, visibleLutOffset);
    const uint32_t visibleEntry = visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t);
    if (visibleLutOffset < HEADER_SIZE || visibleEntry + sizeof(uint32_t) > cached.size()) {
      result.reset();
    } else {
      cached.seek(visibleEntry);
      uint32_t visibleTextOffset = 0;
      serialization::readPod(cached, visibleTextOffset);
      result->visibleTextOffset = visibleTextOffset;
    }
  }
  cached.close();
  return result;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0) return nullptr;
  if (buildActive && currentPage < static_cast<int>(buildLut.size())) {
    HalFile partialFile;
    if (!Storage.openFileForRead("SCT", buildFilePath, partialFile)) return nullptr;
    partialFile.seek(buildLut[currentPage].fileOffset);
    auto page = Page::deserialize(partialFile);
    if (page) page->visibleTextOffset = buildLut[currentPage].visibleTextOffset;
    partialFile.close();
    return page;
  }

  // While a rebuild catches up, pages that belong to a previously committed
  // partial remain readable from the old final file.
  if (currentPage >= pageCount) return nullptr;
  return loadPageAt(currentPage);
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) return nullptr;

  // A live build keeps its write handle open.  Read an already-flushed page
  // through a separate handle so deserialization never moves the append
  // cursor (and therefore cannot corrupt the next checkpoint).
  if (buildActive && page < static_cast<int>(buildLut.size())) {
    HalFile partialFile;
    if (!Storage.openFileForRead("SCT", buildFilePath, partialFile)) return nullptr;
    const auto& entry = buildLut[static_cast<size_t>(page)];
    if (entry.fileOffset < HEADER_SIZE || entry.fileOffset >= partialFile.size()) {
      partialFile.close();
      return nullptr;
    }
    partialFile.seek(entry.fileOffset);
    auto result = Page::deserialize(partialFile);
    if (result) result->visibleTextOffset = entry.visibleTextOffset;
    partialFile.close();
    return result;
  }

  // During a rebuild the committed partial prefix remains readable until the
  // new parser reaches those pages again.
  if (buildActive && (!partial_ || page >= static_cast<int>(partialPageCount_))) return nullptr;
  if (!buildActive && page >= static_cast<int>(pageCount)) return nullptr;
  return loadPageAt(page);
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const ReaderRenderSpec& spec, const uint16_t targetPage) {
  // A page produced by the active parser is already complete and flushed to
  // the checkpoint file. Read it without touching the parser's write cursor;
  // this is both cheaper and safer than inflating/parsing the chapter again.
  if (buildActive && targetPage < buildLut.size()) {
    HalFile partialFile;
    if (Storage.openFileForRead("SCT", buildFilePath, partialFile)) {
      const auto& entry = buildLut[targetPage];
      const uint32_t fileSize = partialFile.size();
      if (entry.fileOffset >= HEADER_SIZE && entry.fileOffset < fileSize) {
        partialFile.seek(entry.fileOffset);
        auto page = Page::deserialize(partialFile);
        if (page) page->visibleTextOffset = entry.visibleTextOffset;
        partialFile.close();
        return page;
      }
      partialFile.close();
    }
    LOG_DBG("SCT", "Active build page %u is not readable yet", static_cast<unsigned>(targetPage));
  }

  // During a rebuild, the committed partial prefix remains authoritative until
  // the active parser reaches the requested page.
  if (partial_ && targetPage < partialPageCount_) {
    if (auto page = loadPageAt(targetPage)) return page;
  }

  // Before the first incremental tick there may be no serialized page yet.
  // Keep the old bounded preview path as a last resort for a caller that needs
  // an immediate page (for example an anchor jump on a cold cache).
  return buildPagePreview(spec.fontId, spec.lineCompression, spec.extraParagraphSpacing, spec.paragraphAlignment,
                          spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled, spec.embeddedStyle,
                          spec.imageRendering, spec.focusReadingEnabled, targetPage);
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const TextBlock& block = *line.getBlock();
          for (size_t i = 0; i < block.wordCount(); ++i) {
            if (!fullText.empty()) fullText += " ";
            fullText.append(block.wordText(i), block.wordTextLen(i));
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    f.close();
    return std::nullopt;
  }

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION) {
    f.close();
    return std::nullopt;
  }
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  f.close();
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  if (buildActive && buildParser) {
    for (const auto& entry : buildParser->getAnchors()) {
      // The parser records an anchor as soon as it sees the element, but its
      // page is not readable until that page-complete callback has flushed the
      // serialized page and LUT entry.  Do not expose a future page to a
      // caller that would immediately fail to load it.
      if (entry.first == anchor && entry.second < builtPageCount) return entry.second;
    }
  }
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    f.close();
    return std::nullopt;
  }
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!buildActive || !buildParser) return std::nullopt;
  for (const auto& entry : buildParser->getAnchors()) {
    // ChapterHtmlSlimParser records anchors as soon as it sees the element,
    // while the containing page is flushed later.  Expose only serialized
    // pages so a caller can immediately load the returned page.
    if (entry.first == anchor && entry.second < builtPageCount) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) return page;
  return getPageForAnchor(anchor);
}

bool Section::hasHtmlCache() const {
  if (!Storage.exists(buildHtmlPath.c_str())) return false;
  HalFile html;
  if (!Storage.openFileForRead("SCT", buildHtmlPath, html)) return false;
  const bool valid = html.size() > 0;
  html.close();
  return valid;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (buildActive) {
    if (page < buildLut.size()) return buildLut[page].visibleTextOffset;
    if (!partial_ || page >= partialPageCount_) return std::nullopt;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) return std::nullopt;

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset = 0;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset == 0 || visibleLutOffset >= fileSize || page >= pageCount) {
    f.close();
    return std::nullopt;
  }
  if (visibleLutOffset + (static_cast<uint32_t>(page) + 1) * sizeof(uint32_t) > fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t));
  uint32_t offset = 0;
  serialization::readPod(f, offset);
  f.close();
  return offset;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  const auto findInEntries = [offset, preferFirstAtOffset](const auto& entries) -> std::optional<uint16_t> {
    if (entries.empty()) return std::nullopt;
    uint16_t result = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      const uint32_t pageOffset = entries[i].visibleTextOffset;
      if (preferFirstAtOffset && pageOffset == offset) return static_cast<uint16_t>(i);
      if (pageOffset > offset) break;
      result = static_cast<uint16_t>(i);
    }
    return result;
  };

  if (buildActive) {
    if (!buildLut.empty() && offset <= buildLut.back().visibleTextOffset) return findInEntries(buildLut);
    if (!partial_) return std::nullopt;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE || pageCount == 0) {
    f.close();
    return std::nullopt;
  }

  // During a rebuild pageCount can already include pages from the active
  // parser, while the on-disk partial still has its smaller watermark. Use the
  // count stored in that file when validating its visible-offset table.
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t cachedPageCount = 0;
  serialization::readPod(f, cachedPageCount);
  if (cachedPageCount == 0) {
    f.close();
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset = 0;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset == 0 || visibleLutOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }
  if (visibleLutOffset + static_cast<uint32_t>(cachedPageCount) * sizeof(uint32_t) > fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(visibleLutOffset);
  uint16_t result = 0;
  for (uint16_t i = 0; i < cachedPageCount; ++i) {
    uint32_t pageOffset = 0;
    serialization::readPod(f, pageOffset);
    if (preferFirstAtOffset && pageOffset == offset) {
      f.close();
      return i;
    }
    if (pageOffset > offset) break;
    result = i;
  }
  if (partial_ && offset > 0) {
    // A partial cache is authoritative only through its last visible offset.
    f.seek(visibleLutOffset + static_cast<uint32_t>(cachedPageCount - 1) * sizeof(uint32_t));
    uint32_t lastOffset = 0;
    serialization::readPod(f, lastOffset);
    if (offset > lastOffset) {
      f.close();
      return std::nullopt;
    }
  }
  f.close();
  return result;
}
