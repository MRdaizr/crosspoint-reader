#include "JsonListParser.h"

#include <Logging.h>
#include <Memory.h>

JsonListParser::JsonListParser(const std::string& items, const std::string* const* fields, const std::string& files,
                               Sink sink, void* context)
    : itemsPath(items), filesPath(files), sink(sink), sinkContext(context) {
  for (size_t i = 0; i < FIELD_COUNT; ++i) selectors[i] = fields[i];
  row.files.reserve(32);
}
std::string JsonListParser::valuePath() const {
  if (!depth) return {};
  const auto& parent = frames[depth - 1];
  const auto segment = parent.array ? std::to_string(parent.index) : parent.key;
  return parent.path.empty() ? segment : parent.path + "." + segment;
}
void JsonListParser::advance() {
  if (depth && frames[depth - 1].array) ++frames[depth - 1].index;
}
void JsonListParser::start(bool array) {
  if (depth >= 32) {
    invalid = true;
    return;
  }
  const auto path = valuePath();
  if (path.size() > 512) {
    invalid = true;
    return;
  }
  if (array && path == itemsPath) sawItems = true;
  if (!array && depth && frames[depth - 1].array && frames[depth - 1].path == itemsPath && rows < 17) {
    rowDepth = depth + 1;
    rowPrefix = path;
    for (auto& field : row.field) field.clear();
    row.files.clear();
    row.files.reserve(32);
  }
  auto& frame = frames[depth++];
  frame.array = array;
  frame.index = 0;
  frame.path = path;
  frame.key.clear();
}
void JsonListParser::end(bool array) {
  if (!depth || frames[depth - 1].array != array) {
    invalid = true;
    return;
  }
  if (rowDepth == depth) {
    sink(sinkContext, row);
    ++rows;
    rowDepth = 0;
  }
  --depth;
  advance();
}
void JsonListParser::scalar(const char* value, size_t size, bool final) {
  if (rowDepth && depth >= rowDepth && rows < 17) {
    const auto path = valuePath();
    const auto relative = path.size() > rowPrefix.size() ? path.substr(rowPrefix.size() + 1) : "";
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
      if (!selectors[i]->empty() && relative == *selectors[i]) {
        const size_t cap = (i == F_URL || i == F_BASE) ? 2048 : 512;
        if (size > cap - row.field[i].size()) {
          invalid = true;
          return;
        }
        row.field[i].append(value, size);
      }
    }
    if (!filesPath.empty() && relative.compare(0, filesPath.size() + 1, filesPath + ".") == 0) {
      // Bundle file arrays are flat and at most 32 paths of 192 bytes.
      const auto tail = relative.substr(filesPath.size() + 1);
      if (tail.find('.') == std::string::npos) {
        const size_t index = frames[depth - 1].index;
        if (index >= 32 || size > 192) {
          invalid = true;
          return;
        }
        if (row.files.size() == index) row.files.emplace_back();
        if (row.files.size() != index + 1 || row.files[index].size() + size > 192) {
          invalid = true;
          return;
        }
        row.files[index].append(value, size);
      }
    }
  }
  if (final) advance();
}
JsonCallbacks JsonListParser::callbacks() {
  JsonCallbacks cb{};
  cb.ctx = this;
  cb.onKey = [](void* ctx, const char* data, size_t n) {
    auto& self = *static_cast<JsonListParser*>(ctx);
    if (!self.depth || n > 128) {
      self.invalid = true;
      return;
    }
    self.frames[self.depth - 1].key.assign(data, n);
  };
  cb.onString =
      cb.onNumber = [](void* ctx, const char* data, size_t n) { static_cast<JsonListParser*>(ctx)->scalar(data, n); };
  cb.onBool = [](void* ctx, bool b) { static_cast<JsonListParser*>(ctx)->scalar(b ? "true" : "false", b ? 4 : 5); };
  cb.onNull = [](void* ctx) { static_cast<JsonListParser*>(ctx)->advance(); };
  cb.onObjectStart = [](void* ctx) { static_cast<JsonListParser*>(ctx)->start(false); };
  cb.onObjectEnd = [](void* ctx) { static_cast<JsonListParser*>(ctx)->end(false); };
  cb.onArrayStart = [](void* ctx) { static_cast<JsonListParser*>(ctx)->start(true); };
  cb.onArrayEnd = [](void* ctx) { static_cast<JsonListParser*>(ctx)->end(true); };
  cb.onStringChunk = [](void* ctx, const char* data, size_t n, bool final) {
    static_cast<JsonListParser*>(ctx)->scalar(data, n, final);
  };
  return cb;
}
bool JsonListParser::parse(Reader reader, void* context) {
  auto parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks());
  auto buffer = makeUniqueNoThrow<char[]>(256);
  if (!parser || !buffer) {
    LOG_ERR("PCAT", "OOM: JSON stream");
    return false;
  }
  while (!invalid && !parser->hasError()) {
    const int n = reader(context, buffer.get(), 256);
    if (n < 0) return false;
    if (!n) break;
    parser->feed(buffer.get(), n);
  }
  parser->feed(" ", 1);
  return !invalid && !parser->hasError() && depth == 0 && sawItems;
}
