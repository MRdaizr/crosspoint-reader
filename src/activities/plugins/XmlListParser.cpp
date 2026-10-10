#include "XmlListParser.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "util/PluginHttpPolicy.h"

const char* XmlListParser::localName(const char* name) {
  const char* p = strrchr(name, '|');
  // The local Expat build does not force XML_NS. Generic feeds select local
  // names, so retain compatibility with its unexpanded prefix spelling too.
  if (!p) p = strrchr(name, ':');
  return p ? p + 1 : name;
}
XmlListParser::XmlListParser(const std::string& item, const std::string& container, const std::string* const* fields,
                             Sink sink, void* context)
    : itemElement(item), containerElement(container), sink(sink), sinkContext(context) {
  for (size_t i = 0; i < FIELD_COUNT; ++i) selectors[i] = fields[i];
  parser = XML_ParserCreateNS(nullptr, '|');
  if (!parser) return;
  XML_SetUserData(parser, this);
  XML_SetElementHandler(
      parser,
      [](void* ctx, const char* name, const char** attr) { static_cast<XmlListParser*>(ctx)->start(name, attr); },
      [](void* ctx, const char* name) { static_cast<XmlListParser*>(ctx)->end(name); });
  XML_SetCharacterDataHandler(
      parser, [](void* ctx, const char* data, int n) { static_cast<XmlListParser*>(ctx)->text(data, n); });
  XML_SetStartDoctypeDeclHandler(parser, [](void* ctx, const char*, const char*, const char*, int) {
    auto& self = *static_cast<XmlListParser*>(ctx);
    self.invalid = true;
    XML_StopParser(self.parser, XML_FALSE);
  });
  XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
}
XmlListParser::~XmlListParser() {
  if (parser) XML_ParserFree(parser);
}
void XmlListParser::start(const char* name, const char** attributes) {
  if (++depth > 32) {
    invalid = true;
    XML_StopParser(parser, XML_FALSE);
    return;
  }
  name = localName(name);
  if (itemDepth == 0 && itemElement == name && rows < MAX_ITEMS) {
    itemDepth = depth;
    row.isDir = false;
    for (auto& field : row.field) field.clear();
  }
  if (!itemDepth) return;
  textElement = name;
  if (!containerElement.empty() && containerElement == name) row.isDir = true;
  for (size_t i = 0; i < FIELD_COUNT; ++i) {
    const auto& selector = *selectors[i];
    const auto at = selector.find('@');
    if (at == std::string::npos || (at > 0 && selector.substr(0, at) != name) || (at == 0 && depth != itemDepth))
      continue;
    for (const char** p = attributes; p && *p; p += 2) {
      if (selector.substr(at + 1) == localName(p[0])) {
        if (strlen(p[1]) > 2048) {
          invalid = true;
          XML_StopParser(parser, XML_FALSE);
          return;
        }
        row.field[i] = p[1];
      }
    }
  }
}
void XmlListParser::text(const char* data, int size) {
  if (!itemDepth || size < 0) return;
  for (size_t i = 0; i < FIELD_COUNT; ++i)
    if (*selectors[i] == textElement) {
      const size_t cap = i == F_URL ? 2048 : 512;
      if (static_cast<size_t>(size) > cap - row.field[i].size()) {
        invalid = true;
        XML_StopParser(parser, XML_FALSE);
        return;
      }
      row.field[i].append(data, size);
    }
}
void XmlListParser::end(const char*) {
  if (depth == itemDepth && itemDepth) {
    std::string resolved;
    auto& url = row.field[F_URL];
    if (urls.resolveUrls && pluginhttp::resolveRedirect(urls.requestUrl, url, resolved)) url = std::move(resolved);
    bool keep = !urls.skipSelf || url != urls.requestUrl;
    if (!row.isDir && !urls.extensions.empty()) {
      std::string lower = url.substr(0, url.find('?'));
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      bool extension = false;
      for (const auto& ext : urls.extensions)
        if (lower.size() >= ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
          extension = true;
      keep = keep && extension;
    }
    if (keep && !row.field[F_TITLE].empty()) {
      sink(sinkContext, row);
      ++rows;
    }
    itemDepth = 0;
  }
  textElement.clear();
  if (depth) --depth;
}
bool XmlListParser::parse(Reader reader, void* context) {
  if (!parser || itemElement.empty()) return false;
  auto buffer = makeUniqueNoThrow<char[]>(256);
  if (!buffer) {
    LOG_ERR("PCAT", "OOM: XML buffer");
    return false;
  }
  while (!invalid) {
    const int n = reader(context, buffer.get(), 256);
    if (n < 0 || XML_Parse(parser, buffer.get(), n, n == 0) != XML_STATUS_OK) return false;
    if (!n) return depth == 0;
  }
  return false;
}
