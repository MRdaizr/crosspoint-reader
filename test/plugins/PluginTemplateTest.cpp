#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "network/ProtectedPaths.h"
#include "util/PluginHttpTemplates.h"
static unsigned checks = 0;
#define CHECK(x)                                   \
  do {                                             \
    ++checks;                                      \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << '\n'; \
      std::exit(1);                                \
    }                                              \
  } while (0)
using Values = std::vector<std::pair<std::string, std::string>>;
static bool lookup(void* ctx, std::string_view key, std::string& value) {
  for (const auto& kv : *static_cast<Values*>(ctx))
    if (kv.first == key) {
      value = kv.second;
      return true;
    }
  return false;
}
int main() {
  using namespace pluginhttp;
  Values v = {{"title", "中文 \"title\" &?/#\n\\"},
              {"n", "10"},
              {"zero", "01"},
              {"flag", "true"},
              {"nil", "null"},
              {"base", "https://example.com/api"},
              {"url", "https://example.com/books/a%20b.epub"},
              {"data", "{\"a\":[1,true]}"},
              {"bad", "{\"a\":1,}"},
              {"path", "/Books/中文 \"title\".epub"},
              {"nested", "{n}"},
              {"crlf", "Bearer x\r\nHost: attacker"}};
  std::string out;
  CHECK(renderTemplate("https://example.com/?q={title}", TemplateContext::Url, lookup, &v, out));
  CHECK(out == "https://example.com/?q=%E4%B8%AD%E6%96%87%20%22title%22%20%26%3F%2F%23%0A%5C");
  CHECK(renderTemplate(R"({"title":"{title}","seconds":{n},"bool":{flag},"nil":{nil},"id":"{n}"})",
                       TemplateContext::Json, lookup, &v, out));
  CHECK(out ==
        "{\"title\":\"中文 \\\"title\\\" &?/#\\u000a\\\\\",\"seconds\":10,\"bool\":true,\"nil\":null,\"id\":\"10\"}");
  CHECK(TemplateJsonValidator(out).validate());
  CHECK(renderTemplate("{\"id\":{zero}}", TemplateContext::Json, lookup, &v, out));
  CHECK(out == "{\"id\":\"01\"}");
  CHECK(renderTemplate("{\"value\":{title}}", TemplateContext::Json, lookup, &v, out));
  CHECK(TemplateJsonValidator(out).validate());
  CHECK(renderTemplate("{\"value\":{raw-json:data}}", TemplateContext::Json, lookup, &v, out));
  CHECK(out == "{\"value\":{\"a\":[1,true]}}");
  CHECK(!renderTemplate("{\"value\":{raw-json:bad}}", TemplateContext::Json, lookup, &v, out));
  CHECK(!renderTemplate("{\"value\":\"{raw-json:data}\"}", TemplateContext::Json, lookup, &v, out));
  CHECK(!renderTemplate("https://example.com/{raw-json:data}", TemplateContext::Url, lookup, &v, out));
  CHECK(renderTemplate("{raw-url:base}/books?q={title}", TemplateContext::Url, lookup, &v, out));
  CHECK(out.starts_with("https://example.com/api/books?q=%E4"));
  CHECK(!renderTemplate("{base}/books", TemplateContext::Url, lookup, &v, out));
  CHECK(renderTemplate("{url}", TemplateContext::Url, lookup, &v, out));
  CHECK(out == v[6].second);
  CHECK(renderTemplate("https://example.com/?url={url}", TemplateContext::Url, lookup, &v, out));
  CHECK(out.find("url=https%3A%2F%2F") != std::string::npos);
  v.emplace_back("unsafeUrl", "https://user:password@example.com/a");
  CHECK(!renderTemplate("{raw-url:unsafeUrl}", TemplateContext::Url, lookup, &v, out));
  CHECK(renderTemplate("user={title}&n={n}", TemplateContext::Form, lookup, &v, out));
  CHECK(out.ends_with("&n=10") && out.find("%26%3F%2F%23%0A") != std::string::npos);
  CHECK(!renderTemplate("{crlf}", TemplateContext::Header, lookup, &v, out));
  CHECK(renderTemplate("Bearer {title}", TemplateContext::Text, lookup, &v, out));
  CHECK(renderTemplate("{path}.meta.json", TemplateContext::Path, lookup, &v, out));
  CHECK(out == v[9].second + ".meta.json" && protectedpaths::isPluginPath(out));
  CHECK(renderTemplate("{\"value\":\"{nested}\"}", TemplateContext::Json, lookup, &v, out));
  CHECK(out == "{\"value\":\"{n}\"}");  // no recursive interpolation of supplied data
  CHECK(!renderTemplate("{missing}", TemplateContext::Text, lookup, &v, out));
  v.emplace_back("big", std::string(8192, '"'));
  CHECK(!renderTemplate("{\"v\":\"{big}\"}", TemplateContext::Json, lookup, &v, out));
  for (auto good : {"0", "-12", "1.25", "1e+2", "true", "null", "[]", "{\"a\":1}"})
    CHECK(TemplateJsonValidator(good).validate());
  for (auto bad : {"01", "+1", "1.", "NaN", "[1,]", "{\"a\":1,}", "\"line\nline\""})
    CHECK(!TemplateJsonValidator(bad).validate());
  std::string depth(17, '[');
  depth += '0';
  depth += std::string(17, ']');
  CHECK(!TemplateJsonValidator(depth).validate());
  std::cout << checks << " context-aware template checks passed\n";
}
