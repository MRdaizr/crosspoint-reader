#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Independent strict reader of the emitted response schema (flat objects plus
// items[]). It checks delimiters/full consumption and decodes escapes; it does
// not reuse the implementation's quote/filter functions or accept raw controls.
struct JsonResponse {
  using Fields = std::map<std::string, std::string>;
  Fields fields;
  std::vector<Fields> items;
  bool valid = false;
  std::string get(const char* key) const {
    const auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
  }
};

class ResponseReader {
  std::string_view input;
  size_t at = 0;
  bool ok = true;
  void space() {
    while (at < input.size() && (input[at] == ' ' || input[at] == '\n' || input[at] == '\r' || input[at] == '\t')) ++at;
  }
  bool take(char c) {
    space();
    if (at < input.size() && input[at] == c) {
      ++at;
      return true;
    }
    return false;
  }
  void need(char c) {
    if (!take(c)) ok = false;
  }
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  std::string string() {
    std::string result;
    need('"');
    while (ok && at < input.size()) {
      const unsigned char c = input[at++];
      if (c == '"') return result;
      if (c < 0x20) {
        ok = false;
        break;
      }
      if (c != '\\') {
        result.push_back(c);
        continue;
      }
      if (at == input.size()) {
        ok = false;
        break;
      }
      switch (input[at++]) {
        case '"':
          result.push_back('"');
          break;
        case '\\':
          result.push_back('\\');
          break;
        case '/':
          result.push_back('/');
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          if (input.size() - at < 4) {
            ok = false;
            break;
          }
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = hex(input[at++]);
            if (digit < 0) {
              ok = false;
              break;
            }
            code = code * 16 + static_cast<unsigned>(digit);
          }
          // Production only escapes ASCII controls as \u00XX; non-ASCII
          // UTF-8 is carried byte-for-byte and is tested independently below.
          if (code > 0x7F) ok = false;
          if (ok) result.push_back(static_cast<char>(code));
          break;
        }
        default:
          ok = false;
      }
    }
    ok = false;
    return result;
  }
  std::string scalar() {
    space();
    if (at < input.size() && input[at] == '"') return string();
    for (const std::string_view literal : {"true", "false", "null"}) {
      if (input.substr(at).starts_with(literal)) {
        at += literal.size();
        return std::string(literal);
      }
    }
    const size_t begin = at;
    while (at < input.size() && input[at] >= '0' && input[at] <= '9') ++at;
    if (begin == at || (at - begin > 1 && input[begin] == '0')) ok = false;
    return std::string(input.substr(begin, at - begin));
  }
  void object(JsonResponse::Fields& fields, std::vector<JsonResponse::Fields>* items) {
    need('{');
    if (take('}')) return;
    do {
      if (!ok) return;
      const std::string key = string();
      need(':');
      if (key == "items" && items) {
        need('[');
        if (!take(']')) {
          do {
            if (!ok) return;
            items->emplace_back();
            object(items->back(), nullptr);
          } while (take(','));
          need(']');
        }
      } else {
        if (fields.contains(key)) ok = false;
        fields[key] = scalar();
      }
    } while (take(','));
    need('}');
  }

 public:
  explicit ResponseReader(std::string_view text) : input(text) {}
  JsonResponse parse() {
    JsonResponse reply;
    object(reply.fields, &reply.items);
    space();
    reply.valid = ok && at == input.size();
    return reply;
  }
};
