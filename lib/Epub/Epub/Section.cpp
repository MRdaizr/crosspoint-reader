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
constexpr uint8_t SECTION_FILE_VERSION = 32;
constexpr size_t MIN_INCREMENTAL_FREE_HEAP = 48 * 1024;
constexpr size_t MIN_INCREMENTAL_MAX_ALLOC = 32 * 1024;
constexpr uint16_t INCREMENTAL_PARSE_BUFFER_SIZE = 256;
constexpr uint32_t BUILD_CHECKPOINT_MAGIC = 0x43504231;  // CPB1
// ImageBlock page records gained a serialized source href in section v32. An
// old .building file must not be resumed and have new-format pages appended to
// its old-format prefix, so invalidate the incremental checkpoint as well.
constexpr uint16_t BUILD_CHECKPOINT_VERSION = 3;
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
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

Section::~Section() { preserveIncrementalBuild(); }

uint32_t Section::onIncrementalPageComplete(std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                                             const uint16_t listItemIndex, const uint32_t visibleTextOffset) {
  if (!buildFile) {
    LOG_ERR("SCT", "Build file not open for page %d", pageCount);
    return 0;
  }

  const uint32_t position = buildFile.position();
  if (!page->serialize(buildFile)) {
    LOG_ERR("SCT", "Failed to serialize incremental page %d", pageCount);
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
  pageCount++;
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
}

void Section::discardIncrementalBuild() {
  preserveIncrementalBuild();
  if (!buildHtmlPath.empty() && Storage.exists(buildHtmlPath.c_str())) {
    Storage.remove(buildHtmlPath.c_str());
  }
  if (Storage.exists(buildFilePath.c_str())) {
    Storage.remove(buildFilePath.c_str());
  }
  if (Storage.exists(buildIndexPath.c_str())) {
    Storage.remove(buildIndexPath.c_str());
  }
  resumePageCount = 0;
}

bool Section::finishIncrementalBuild() {
  if (!buildFile || !buildParser) {
    return false;
  }

  const uint32_t lutOffset = buildFile.position();
  for (const auto& entry : buildLut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write incremental page LUT");
      return false;
    }
    serialization::writePod(buildFile, entry.fileOffset);
  }

  const uint32_t anchorMapOffset = buildFile.position();
  const auto& anchors = buildParser->getAnchors();
  serialization::writePod(buildFile, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
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

  buildFile.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(pageCount));
  serialization::writePod(buildFile, pageCount);
  serialization::writePod(buildFile, lutOffset);
  serialization::writePod(buildFile, anchorMapOffset);
  serialization::writePod(buildFile, paragraphLutOffset);
  serialization::writePod(buildFile, liLutOffset);
  serialization::writePod(buildFile, visibleLutOffset);
  buildFile.close();

  buildParser.reset();
  if (buildCssParser) {
    buildCssParser->clear();
    buildCssParser = nullptr;
  }
  Storage.remove(buildHtmlPath.c_str());
  Storage.remove(buildIndexPath.c_str());
  Storage.remove(filePath.c_str());
  if (!Storage.rename(buildFilePath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to commit incremental section cache");
    return false;
  }
  buildLut.clear();
  buildActive = false;
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
  serialization::writePod(target, SECTION_FILE_VERSION);
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
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

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
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const std::function<void()>& popupFn, const std::function<void(uint8_t)>& progressFn) {
  pageCount = 0;
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
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
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
  if (checkpoint.read(&header, sizeof(header)) != sizeof(header) || header.magic != BUILD_CHECKPOINT_MAGIC ||
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

  pageCount = static_cast<uint16_t>(buildLut.size());
  resumePageCount = pageCount;
  buildActive = true;
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
  discardIncrementalBuild();
  pageCount = 0;

  const auto sectionsDir = epub->getCachePath() + "/sections";
  Storage.mkdir(sectionsDir.c_str());
  Storage.remove(buildFilePath.c_str());
  Storage.remove(buildHtmlPath.c_str());

  const auto localPath = epub->getSpineItem(spineIndex).href;
  bool streamed = false;
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

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (buildActive) {
    if (currentPage < 0 || currentPage >= static_cast<int>(buildLut.size())) {
      return nullptr;
    }
    HalFile partialFile;
    if (!Storage.openFileForRead("SCT", buildFilePath, partialFile)) {
      return nullptr;
    }
    partialFile.seek(buildLut[currentPage].fileOffset);
    auto page = Page::deserialize(partialFile);
    if (page) {
      page->visibleTextOffset = buildLut[currentPage].visibleTextOffset;
    }
    partialFile.close();
    return page;
  }

  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 5);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  if (page) {
    file.seek(HEADER_SIZE - sizeof(uint32_t));
    uint32_t visibleTextOffset = 0;
    serialization::readPod(file, visibleTextOffset);
    file.seek(visibleTextOffset + static_cast<uint32_t>(currentPage) * sizeof(uint32_t));
    serialization::readPod(file, visibleTextOffset);
    page->visibleTextOffset = visibleTextOffset;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = this->loadPageFromSectionFile();
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
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
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
      return page;
    }
  }

  return std::nullopt;
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
    if (page >= buildLut.size()) return std::nullopt;
    return buildLut[page].visibleTextOffset;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) return std::nullopt;

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset = 0;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset == 0 || visibleLutOffset >= fileSize || page >= pageCount) return std::nullopt;
  if (visibleLutOffset + (static_cast<uint32_t>(page) + 1) * sizeof(uint32_t) > fileSize) return std::nullopt;

  f.seek(visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t));
  uint32_t offset = 0;
  serialization::readPod(f, offset);
  return offset;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset) const {
  if (buildActive) {
    if (buildLut.empty()) return std::nullopt;
    uint16_t result = 0;
    for (uint16_t i = 0; i < buildLut.size(); ++i) {
      if (buildLut[i].visibleTextOffset > offset) break;
      result = i;
    }
    return result;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE || pageCount == 0) return std::nullopt;

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset = 0;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset == 0 || visibleLutOffset >= fileSize) return std::nullopt;
  if (visibleLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t) > fileSize) return std::nullopt;

  f.seek(visibleLutOffset);
  uint16_t result = 0;
  for (uint16_t i = 0; i < pageCount; ++i) {
    uint32_t pageOffset = 0;
    serialization::readPod(f, pageOffset);
    if (pageOffset > offset) break;
    result = i;
  }
  return result;
}
