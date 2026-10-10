#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_IDENTIFIER,
    IN_META_TEXT,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  const bool metadataOnly;
  const bool collectExtendedMetadata;
  bool metadataComplete = false;
  bool parseFailed = false;
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;
  struct ExtendedMetadata;
  std::unique_ptr<ExtendedMetadata> extended;
  HalFile tempItemStore;
  std::string coverItemId;
  bool hasExplicitStartReference = false;

  // Index for fast idref→href lookup (built for every non-empty manifest)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  // Lazily allocated only when writing manifest/spine entries. Metadata scans
  // must not allocate the deque's initial map and block.
  std::unique_ptr<std::deque<ItemIndexEntry>> itemIndex;
  bool useItemIndex = false;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string isbn;
  std::string asin;
  std::string series;
  std::optional<float> seriesIndex;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, bool metadataOnly = false, bool collectExtendedMetadata = false);
  ~ContentOpfParser() override;

  bool setup();
  // A short write can mean success (metadata early stop) or an XML failure.
  // ZIP callers using allowEarlyStop must check this separately.
  bool succeeded() const { return !parseFailed && (metadataOnly ? metadataComplete : remainingSize == 0); }

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
