#pragma once
#include <expat.h>

#include <string>
#include <vector>
class XmlListParser {
 public:
  static constexpr size_t MAX_ITEMS = 32;
  enum Field { F_URL, F_TITLE, F_AUTHOR, F_ID, FIELD_COUNT };
  struct RawItem {
    std::string field[FIELD_COUNT];
    bool isDir = false;
  };
  struct UrlOptions {
    std::string requestUrl;
    bool skipSelf = false, resolveUrls = false;
    std::vector<std::string> extensions;
  };
  using Reader = int (*)(void*, char*, size_t);
  using Sink = void (*)(void*, RawItem&);
  XmlListParser(const std::string& item, const std::string& container, const std::string* const* fields, Sink sink,
                void* context);
  ~XmlListParser();
  void setUrlOptions(UrlOptions options) { urls = std::move(options); }
  bool parse(Reader reader, void* context);

 private:
  XML_Parser parser = nullptr;
  std::string itemElement, containerElement, textElement;
  const std::string* selectors[FIELD_COUNT];
  RawItem row;
  UrlOptions urls;
  Sink sink;
  void* sinkContext;
  size_t depth = 0, itemDepth = 0, rows = 0;
  bool invalid = false;
  static const char* localName(const char* name);
  void start(const char* name, const char** attributes);
  void end(const char* name);
  void text(const char* data, int size);
};
