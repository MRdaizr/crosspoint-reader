#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <XmlParserUtils.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t MAX_METADATA_TEXT = 512;
constexpr size_t MAX_METADATA_CANDIDATES = 8;

bool isXmlWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Only metadata-only reads normalize whitespace. Normal book-cache parsing
// retains the existing text formatting and cache version.
void appendMetadataText(std::string& out, const XML_Char* text, int len, bool& spacePending,
                        bool* separatorPending = nullptr) {
  for (int i = 0; i < len && out.size() < MAX_METADATA_TEXT; ++i) {
    if (isXmlWhitespace(text[i])) {
      spacePending = true;
      continue;
    }
    if (separatorPending && *separatorPending) {
      if (out.size() + 3 > MAX_METADATA_TEXT) return;
      out.append(", ");
      *separatorPending = false;
      spacePending = false;
    } else if (spacePending && !out.empty()) {
      if (out.size() + 2 > MAX_METADATA_TEXT) return;
      out.push_back(' ');
    }
    spacePending = false;
    out.push_back(text[i]);
  }
}

std::string boundedValue(const char* value) {
  if (!value) return {};
  size_t size = 0;
  while (size < MAX_METADATA_TEXT && value[size]) ++size;
  return std::string(value, size);
}

std::string lowerAscii(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::optional<float> parseFiniteFloat(const std::string& value) {
  if (value.empty()) return std::nullopt;
  char* end = nullptr;
  errno = 0;
  const float parsed = std::strtof(value.c_str(), &end);
  if (end == value.c_str() || errno == ERANGE || !std::isfinite(parsed)) return std::nullopt;
  while (isXmlWhitespace(*end)) ++end;
  return *end == '\0' ? std::optional<float>(parsed) : std::nullopt;
}

bool hasIdentifierPrefix(const std::string& text, const char* type) {
  const std::string lower = lowerAscii(text);
  const std::string urn = std::string("urn:") + type + ":";
  if (lower.rfind(urn, 0) == 0) return true;
  const size_t length = strlen(type);
  return lower.compare(0, length, type) == 0 && lower.size() > length &&
         (lower[length] == ':' || isXmlWhitespace(lower[length]));
}

std::string stripIdentifierPrefix(const std::string& text, const char* type) {
  if (!hasIdentifierPrefix(text, type)) return text;
  size_t offset = lowerAscii(text).rfind("urn:", 0) == 0 ? 4 + strlen(type) : strlen(type);
  while (offset < text.size() && (text[offset] == ':' || isXmlWhitespace(text[offset]))) ++offset;
  return text.substr(offset);
}

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}
}  // namespace

// Fixed bounded tables live on the heap only for loadSyncMetadata, never for
// normal rendering or core metadata. Refinements may precede their targets.
struct ContentOpfParser::ExtendedMetadata {
  struct Identifier {
    std::string id;
    std::string text;
    std::string scheme;
    std::string type;
  };
  struct Collection {
    std::string id;
    std::string title;
    std::optional<float> index;
    bool isSeries = false;
  };
  std::array<Identifier, MAX_METADATA_CANDIDATES> identifiers;
  std::array<Collection, MAX_METADATA_CANDIDATES> collections;
  size_t identifierCount = 0;
  size_t collectionCount = 0;
  std::string text;
  std::string id;
  std::string scheme;
  std::string property;
  std::string refines;
  std::string calibreSeries;
  std::optional<float> calibreSeriesIndex;

  Identifier* identifier(const std::string& target) {
    for (size_t i = 0; i < identifierCount; ++i) {
      if (!target.empty() && identifiers[i].id == target) return &identifiers[i];
    }
    if (identifierCount == identifiers.size()) return nullptr;
    auto* result = &identifiers[identifierCount++];
    result->id = target;
    return result;
  }
  Collection* collection(const std::string& target) {
    if (target.empty()) return nullptr;
    for (size_t i = 0; i < collectionCount; ++i) {
      if (collections[i].id == target) return &collections[i];
    }
    if (collectionCount == collections.size()) return nullptr;
    auto* result = &collections[collectionCount++];
    result->id = target;
    return result;
  }
};

ContentOpfParser::ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, size_t xmlSize,
                                   BookMetadataCache* cache, bool metadataOnly, bool collectExtendedMetadata)
    : cachePath(cachePath),
      baseContentPath(baseContentPath),
      remainingSize(xmlSize),
      cache(cache),
      metadataOnly(metadataOnly),
      collectExtendedMetadata(collectExtendedMetadata) {}

bool ContentOpfParser::setup() {
  if (collectExtendedMetadata) {
    extended = makeUniqueNoThrow<ExtendedMetadata>();
    if (!extended) {
      LOG_ERR("COF", "Could not allocate extended metadata");
      return false;
    }
  }
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (metadataOnly || !cache) return;
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || (metadataComplete && metadataOnly)) return 0;
  if (size > remainingSize) {
    parseFailed = true;
    return 0;
  }

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      parseFailed = true;
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      parseFailed = true;
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
    if (metadataOnly && metadataComplete) {
      const size_t processed = size - remainingInBuffer;
      return processed < size ? processed : size - 1;
    }
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  if (self->metadataOnly && self->metadataComplete) return;
  if (self->metadataOnly && self->state == IN_PACKAGE &&
      (xmlLocalNameEquals(name, "manifest") || xmlLocalNameEquals(name, "spine") ||
       xmlLocalNameEquals(name, "guide"))) {
    // Missing metadata is not a successful probe, and must never reach the
    // cache-writing handlers below.
    self->parseFailed = true;
    XML_StopParser(self->parser, XML_FALSE);
    return;
  }

  if (self->state == START && xmlLocalNameEquals(name, "package")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "metadata")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "title")) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
      self->metadataSpacePending = false;
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_BOOK_AUTHOR;
    self->metadataSpacePending = false;
    self->authorSeparatorPending = !self->author.empty();
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "language")) {
    self->state = IN_BOOK_LANGUAGE;
    self->metadataSpacePending = false;
    return;
  }

  if (self->extended && self->state == IN_METADATA && xmlLocalNameEquals(name, "identifier")) {
    auto& value = *self->extended;
    value.text.clear();
    value.id.clear();
    value.scheme.clear();
    for (int i = 0; atts[i]; i += 2) {
      if (xmlLocalNameEquals(atts[i], "id")) value.id = boundedValue(atts[i + 1]);
      if (xmlLocalNameEquals(atts[i], "scheme")) value.scheme = lowerAscii(boundedValue(atts[i + 1]));
    }
    self->metadataSpacePending = false;
    self->state = IN_BOOK_IDENTIFIER;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_MANIFEST;
    if (self->cache && !self->metadataOnly &&
        !Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_SPINE;
    if (self->cache && !self->metadataOnly &&
        !Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Always sort the manifest index: even medium books otherwise rescan the
    // temporary item file for every spine itemref.
    if (self->itemIndex && !self->itemIndex->empty()) {
      std::sort(self->itemIndex->begin(), self->itemIndex->end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex->size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (self->cache && !self->metadataOnly &&
        !Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "meta")) {
    std::string metaName;
    std::string content;
    if (self->extended) {
      self->extended->property.clear();
      self->extended->refines.clear();
      self->extended->id.clear();
      self->extended->scheme.clear();
      self->extended->text.clear();
    }
    for (int i = 0; atts[i]; i += 2) {
      if (xmlLocalNameEquals(atts[i], "name")) {
        metaName = boundedValue(atts[i + 1]);
      } else if (xmlLocalNameEquals(atts[i], "content")) {
        content = boundedValue(atts[i + 1]);
      } else if (self->extended) {
        auto& value = *self->extended;
        if (xmlLocalNameEquals(atts[i], "property")) value.property = lowerAscii(boundedValue(atts[i + 1]));
        if (xmlLocalNameEquals(atts[i], "id")) value.id = boundedValue(atts[i + 1]);
        if (xmlLocalNameEquals(atts[i], "scheme")) value.scheme = lowerAscii(boundedValue(atts[i + 1]));
        if (xmlLocalNameEquals(atts[i], "refines")) {
          value.refines = boundedValue(atts[i + 1]);
          if (!value.refines.empty() && value.refines.front() == '#') value.refines.erase(0, 1);
        }
      }
    }
    if (metaName == "cover") self->coverItemId = content;
    if (self->extended) {
      auto& value = *self->extended;
      const std::string lowerName = lowerAscii(metaName);
      if (lowerName == "calibre:series" && value.calibreSeries.empty()) value.calibreSeries = content;
      if (lowerName == "calibre:series_index" && !value.calibreSeriesIndex.has_value()) {
        value.calibreSeriesIndex = parseFiniteFloat(content);
      }
      if (value.property == "belongs-to-collection" || value.property == "collection-type" ||
          value.property == "group-position" || value.property == "identifier-type") {
        self->state = IN_META_TEXT;
        self->metadataSpacePending = false;
      }
    }
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "item")) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      if (!self->itemIndex) {
        self->itemIndex = makeUniqueNoThrow<std::deque<ItemIndexEntry>>();
        if (!self->itemIndex) {
          LOG_ERR("COF", "Could not allocate manifest index");
          self->parseFailed = true;
          XML_StopParser(self->parser, XML_FALSE);
          return;
        }
      }
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex->push_back(entry);
    }

    // Write items down to SD card
    if (self->tempItemStore) {
      serialization::writeString(self->tempItemStore, itemId);
      serialization::writeString(self->tempItemStore, href);
    }

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      if (self->cssFiles.empty()) self->cssFiles.reserve(8);
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && xmlLocalNameEquals(name, "itemref")) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            // Fast path: binary search
            uint32_t targetHash = fnvHash(idref);
            uint16_t targetLen = static_cast<uint16_t>(idref.size());

            auto it = std::lower_bound(self->itemIndex->begin(), self->itemIndex->end(),
                                       ItemIndexEntry{targetHash, targetLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            // Check for match (may need to check a few due to hash collisions)
            while (it != self->itemIndex->end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              std::string itemId;
              serialization::readString(self->tempItemStore, itemId);
              if (itemId == idref) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            // Slow path: linear scan (for small manifests, keeps original behavior)
            // TODO: This lookup is slow as need to scan through all items each time.
            //       It can take up to 200ms per item when getting to 1500 items.
            self->tempItemStore.seek(0);
            std::string itemId;
            while (self->tempItemStore.available()) {
              serialization::readString(self->tempItemStore, itemId);
              serialization::readString(self->tempItemStore, href);
              if (itemId == idref) {
                found = true;
                break;
              }
            }
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "reference")) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      // `text` is ambiguous in EPUB 2 guides (often present on every
      // chapter). Only an explicit `start` reference identifies the initial
      // reading position.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = true;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  if (self->metadataOnly && self->metadataComplete) return;

  if (self->state == IN_BOOK_IDENTIFIER || self->state == IN_META_TEXT) {
    appendMetadataText(self->extended->text, s, len, self->metadataSpacePending);
    return;
  }
  if (self->metadataOnly) {
    if (self->state == IN_BOOK_TITLE) appendMetadataText(self->title, s, len, self->metadataSpacePending);
    if (self->state == IN_BOOK_AUTHOR) {
      appendMetadataText(self->author, s, len, self->metadataSpacePending, &self->authorSeparatorPending);
    }
    if (self->state == IN_BOOK_LANGUAGE) appendMetadataText(self->language, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");  // Add separator for multiple authors
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  if (self->metadataOnly && self->metadataComplete) return;

  if (self->state == IN_BOOK_IDENTIFIER && xmlLocalNameEquals(name, "identifier")) {
    auto& value = *self->extended;
    if (auto* identifier = value.identifier(value.id)) {
      identifier->text = std::move(value.text);
      identifier->scheme = std::move(value.scheme);
    }
    self->state = IN_METADATA;
    return;
  }
  if (self->state == IN_META_TEXT && xmlLocalNameEquals(name, "meta")) {
    auto& value = *self->extended;
    if (value.property == "identifier-type") {
      if (auto* identifier = value.identifier(value.refines)) identifier->type = lowerAscii(value.text);
    } else {
      const std::string& target = value.property == "belongs-to-collection" ? value.id : value.refines;
      if (auto* collection = value.collection(target)) {
        if (value.property == "belongs-to-collection") collection->title = std::move(value.text);
        if (value.property == "collection-type") collection->isSeries = lowerAscii(value.text) == "series";
        if (value.property == "group-position") collection->index = parseFiniteFloat(value.text);
      }
    }
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_SPINE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && xmlLocalNameEquals(name, "title")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && xmlLocalNameEquals(name, "language")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "metadata")) {
    if (self->extended) {
      auto& value = *self->extended;
      for (size_t i = 0; i < value.identifierCount; ++i) {
        const auto& identifier = value.identifiers[i];
        const bool isIsbn = identifier.scheme == "isbn" || identifier.scheme == "isbn10" ||
                            identifier.scheme == "isbn13" || identifier.scheme == "isbn-10" ||
                            identifier.scheme == "isbn-13" || identifier.type == "isbn" || identifier.type == "15" ||
                            identifier.type == "02" || hasIdentifierPrefix(identifier.text, "isbn");
        const bool isAsin = identifier.scheme == "asin" || identifier.scheme == "amazon" || identifier.type == "asin" ||
                            identifier.type == "amazon" || hasIdentifierPrefix(identifier.text, "asin");
        if (self->isbn.empty() && isIsbn) self->isbn = stripIdentifierPrefix(identifier.text, "isbn");
        if (self->asin.empty() && isAsin) self->asin = stripIdentifierPrefix(identifier.text, "asin");
      }
      if (!value.calibreSeries.empty()) {
        self->series = std::move(value.calibreSeries);
        self->seriesIndex = value.calibreSeriesIndex;
      } else {
        for (size_t i = 0; i < value.collectionCount; ++i) {
          const auto& collection = value.collections[i];
          if (!collection.isSeries || collection.title.empty()) continue;
          self->series = collection.title;
          self->seriesIndex = collection.index;
          break;
        }
      }
    }
    self->state = IN_PACKAGE;
    self->metadataComplete = true;
    if (self->metadataOnly) XML_StopParser(self->parser, XML_TRUE);
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "package")) {
    self->state = START;
    return;
  }
}
