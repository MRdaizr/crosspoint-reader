#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "activities/plugins/JsonListParser.h"
#include "activities/plugins/XmlListParser.h"
static unsigned checks = 0;
#define CHECK(x)                                   \
  do {                                             \
    ++checks;                                      \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << '\n'; \
      std::exit(1);                                \
    }                                              \
  } while (0)
struct Input {
  std::string text;
  size_t pos = 0;
  size_t chunk = 7;
};
static int read(void* context, char* out, size_t cap) {
  auto& in = *static_cast<Input*>(context);
  const auto n = std::min({cap, in.chunk, in.text.size() - in.pos});
  memcpy(out, in.text.data() + in.pos, n);
  in.pos += n;
  return int(n);
}
static bool json(const std::string& text, std::vector<JsonListParser::Row>& rows, const std::string& items = "items") {
  std::string names[JsonListParser::FIELD_COUNT] = {"title", "author", "id", "url", "version", "base"};
  const std::string* fields[JsonListParser::FIELD_COUNT];
  for (size_t i = 0; i < JsonListParser::FIELD_COUNT; ++i) fields[i] = &names[i];
  auto parser = std::make_unique<JsonListParser>(
      items, fields, "files",
      [](void* ctx, JsonListParser::Row& row) { static_cast<std::vector<JsonListParser::Row>*>(ctx)->push_back(row); },
      &rows);
  Input in{text};
  return parser->parse(read, &in);
}
static bool xml(const std::string& text, std::vector<XmlListParser::RawItem>& rows) {
  std::string names[XmlListParser::FIELD_COUNT] = {"href", "displayname", "author", "@id"};
  const std::string* fields[XmlListParser::FIELD_COUNT];
  for (size_t i = 0; i < XmlListParser::FIELD_COUNT; ++i) fields[i] = &names[i];
  auto parser = std::make_unique<XmlListParser>(
      "response", "collection", fields,
      [](void* ctx, XmlListParser::RawItem& row) {
        static_cast<std::vector<XmlListParser::RawItem>*>(ctx)->push_back(row);
      },
      &rows);
  XmlListParser::UrlOptions options;
  options.requestUrl = "https://example.com/books/";
  options.resolveUrls = true;
  options.extensions = {".epub"};
  parser->setUrlOptions(std::move(options));
  Input in{text};
  return parser->parse(read, &in);
}
int main() {
  std::vector<JsonListParser::Row> jr;
  CHECK(json(
      R"({"items":[{"title":"中文书名","id":17,"url":"https://example.com/a.epub","files":["main.js","device.json"]}]})",
      jr));
  CHECK(jr.size() == 1 && jr[0].field[0] == "中文书名" && jr[0].field[2] == "17");
  CHECK(jr[0].files.size() == 2 && jr[0].files[1] == "device.json");
  jr.clear();
  CHECK(json("[]", jr, ""));
  CHECK(jr.empty());
  jr.clear();
  CHECK(!json("{\"items\":[{\"title\":\"unterminated", jr));
  jr.clear();
  CHECK(!json("{\"items\":[{\"title\":\"" + std::string(513, 'x') + "\"}]}", jr));
  jr.clear();
  CHECK(!json("{\"items\":[{\"files\":[\"" + std::string(193, 'x') + "\"]}]}", jr));
  jr.clear();
  std::string many = "{\"items\":[";
  for (int i = 0; i < 40; ++i) many += (i ? "," : "") + std::string("{\"title\":\"row\"}");
  many += "]}";
  CHECK(json(many, jr));
  CHECK(jr.size() == 17);  // one look-ahead row, max page 16
  std::vector<XmlListParser::RawItem> xr;
  CHECK(xml(
      R"(<d:multistatus xmlns:d="DAV:"><d:response id="7"><d:href>one.epub</d:href><d:displayname>Book &amp; title</d:displayname></d:response><d:response><d:href>folder/</d:href><d:displayname>Folder</d:displayname><d:collection/></d:response><d:response><d:href>x.exe</d:href><d:displayname>Skip</d:displayname></d:response></d:multistatus>)",
      xr));
  CHECK(xr.size() == 2 && xr[0].field[0] == "https://example.com/books/one.epub");
  CHECK(xr[0].field[1] == "Book & title" && xr[0].field[3] == "7" && xr[1].isDir);
  xr.clear();
  CHECK(!xml("<!DOCTYPE x [<!ENTITY a 'bomb'>]><x>&a;</x>", xr));
  xr.clear();
  CHECK(!xml("<x><response>", xr));
  xr.clear();
  CHECK(!xml("<x><response><displayname>" + std::string(513, 'a') + "</displayname></response></x>", xr));
  std::cout << checks << " streaming JSON/XML checks passed\n";
}
