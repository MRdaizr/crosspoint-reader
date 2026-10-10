#pragma once
#include <StreamingJsonParser.h>

#include <algorithm>
#include <string>
#include <vector>

// Catalog streaming adapter: only a bounded row survives each JSON item.
// The parser object (path stack + 512-byte tokenizer) is allocated once with
// makeUniqueNoThrow by the activity; no full feed document is built in RAM.
class JsonListParser {
 public:
  enum Field { F_TITLE, F_AUTHOR, F_ID, F_URL, F_VERSION, F_BASE, FIELD_COUNT };
  struct Row {
    std::string field[FIELD_COUNT];
    std::vector<std::string> files;
  };
  using Reader = int (*)(void*, char*, size_t);
  using Sink = void (*)(void*, Row&);
  JsonListParser(const std::string& items, const std::string* const* fields, const std::string& files, Sink sink,
                 void* context);
  bool parse(Reader reader, void* context);

 private:
  struct Frame {
    bool array = false;
    size_t index = 0;
    std::string path, key;
  };
  Frame frames[32];
  size_t depth = 0, rowDepth = 0, rows = 0;
  bool invalid = false, sawItems = false;
  std::string itemsPath, filesPath, rowPrefix;
  const std::string* selectors[FIELD_COUNT];
  Row row;
  Sink sink;
  void* sinkContext;
  std::string valuePath() const;
  void start(bool array);
  void end(bool array);
  void scalar(const char* value, size_t size, bool final = true);
  void advance();
  JsonCallbacks callbacks();
};
