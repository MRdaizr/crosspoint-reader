#pragma once
#include <string_view>

#include "PluginHttpPolicy.h"

namespace pluginhttp {
enum class TemplateContext { Url, Json, Form, Header, Path, Text };
inline constexpr size_t MAX_TEMPLATE_BYTES = 8192;
using TemplateLookup = bool (*)(void*, std::string_view, std::string&);
inline std::string percentEncode(std::string_view value) {
  static constexpr char TEMPLATE_HEX[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(std::min(MAX_TEMPLATE_BYTES, value.size() * 3));
  for (unsigned char c : value) {
    if (out.size() > MAX_TEMPLATE_BYTES - 3) return {};
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~')
      out += static_cast<char>(c);
    else {
      out += '%';
      out += TEMPLATE_HEX[c >> 4];
      out += TEMPLATE_HEX[c & 15];
    }
  }
  return out;
}
inline std::string jsonEscape(std::string_view value) {
  static constexpr char TEMPLATE_HEX[] = "0123456789abcdef";
  std::string out;
  out.reserve(std::min(MAX_TEMPLATE_BYTES, value.size() * 2));
  for (unsigned char c : value) {
    if (out.size() > MAX_TEMPLATE_BYTES - 6) return {};
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32) {
      out += "\\u00";
      out += TEMPLATE_HEX[c >> 4];
      out += TEMPLATE_HEX[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out;
}
// Small strict validator used only for explicit raw-json and final JSON bodies.
// No allocation, max depth 16 / 1024 nodes / 8KB; no NaN or Infinity.
class TemplateJsonValidator {
 public:
  explicit TemplateJsonValidator(std::string_view text) : text(text) {}
  bool validate() { return text.size() <= MAX_TEMPLATE_BYTES && value(0) && (space(), pos == text.size()); }
  static bool scalar(std::string_view text) {
    if (text == "true" || text == "false" || text == "null") return true;
    if (text.empty() || text.size() > 64 || (text[0] != '-' && (text[0] < '0' || text[0] > '9'))) return false;
    TemplateJsonValidator v(text);
    return v.number() && v.pos == text.size();
  }

 private:
  std::string_view text;
  size_t pos = 0, nodes = 0;
  void space() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\r' || text[pos] == '\n' || text[pos] == '\t'))
      ++pos;
  }
  bool take(char c) {
    if (pos < text.size() && text[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }
  bool digits() {
    const auto first = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    return pos != first;
  }
  bool number() {
    take('-');
    if (!take('0') && !digits()) return false;
    if (take('.') && !digits()) return false;
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      if (!digits()) return false;
    }
    return true;
  }
  bool string() {
    if (!take('"')) return false;
    while (pos < text.size()) {
      const unsigned char c = text[pos++];
      if (c == '"') return true;
      if (c < 32) return false;
      if (c == '\\') {
        if (pos == text.size()) return false;
        const char escape = text[pos++];
        if (escape == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (pos == text.size()) return false;
            const char h = text[pos++];
            if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
          }
        } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
          return false;
      }
    }
    return false;
  }
  bool value(size_t depth) {
    if (depth > 16 || ++nodes > 1024) return false;
    space();
    if (pos == text.size()) return false;
    if (text[pos] == '"') return string();
    if (take('{')) {
      space();
      if (take('}')) return true;
      do {
        space();
        if (!string()) return false;
        space();
        if (!take(':') || !value(depth + 1)) return false;
        space();
      } while (take(','));
      return take('}');
    }
    if (take('[')) {
      space();
      if (take(']')) return true;
      do {
        if (!value(depth + 1)) return false;
        space();
      } while (take(','));
      return take(']');
    }
    for (auto word : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
      if (text.substr(pos, word.size()) == word) {
        pos += word.size();
        return true;
      }
    }
    return number();
  }
};
inline TemplateContext bodyTemplateContext(std::string_view text) {
  const auto pos = text.find_first_not_of(" \r\n\t");
  return pos != std::string_view::npos && (text[pos] == '{' || text[pos] == '[' || text[pos] == '"')
             ? TemplateContext::Json
             : TemplateContext::Form;
}
// Single pass: braces introduced by a value are never interpreted as templates.
inline bool renderTemplate(std::string_view tpl, TemplateContext context, TemplateLookup lookup, void* state,
                           std::string& out) {
  out.clear();
  if (tpl.size() > MAX_TEMPLATE_BYTES) return false;
  out.reserve(tpl.size());
  bool quoted = false, escaped = false;
  for (size_t pos = 0; pos < tpl.size();) {
    bool replaced = false;
    if (tpl[pos] == '{') {
      const auto end = tpl.find('}', pos + 1);
      if (end != std::string_view::npos && end - pos <= 96) {
        auto key = tpl.substr(pos + 1, end - pos - 1);
        bool rawUrl = key.starts_with("raw-url:"), rawJson = key.starts_with("raw-json:");
        if (rawUrl)
          key.remove_prefix(8);
        else if (rawJson)
          key.remove_prefix(9);
        const bool token = !key.empty() &&
                           key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") ==
                               std::string_view::npos;
        if (token) {
          std::string value, encoded;
          if (!lookup(state, key, value) || value.size() > MAX_TEMPLATE_BYTES) return false;
          if (rawUrl) {
            if (context != TemplateContext::Url || !validUrl(value)) return false;
            encoded = value;
          } else if (rawJson) {
            if (context != TemplateContext::Json || quoted || !TemplateJsonValidator(value).validate()) return false;
            encoded = value;
          } else if (context == TemplateContext::Url || context == TemplateContext::Form) {
            // Restricted baseline compatibility: only an entire {url} template
            // may carry an already validated full feed URL. All other variables
            // (including query_raw) are encoded; base URLs use raw-url explicitly.
            if (context == TemplateContext::Url && pos == 0 && end + 1 == tpl.size() && key == "url") {
              if (!validUrl(value)) return false;
              encoded = value;
            } else
              encoded = percentEncode(value);
          } else if (context == TemplateContext::Json) {
            if (!quoted && TemplateJsonValidator::scalar(value))
              encoded = value;
            else {
              encoded = jsonEscape(value);
              if (!quoted) encoded = '"' + encoded + '"';
            }
          } else
            encoded = value;
          if ((!value.empty() && encoded.empty()) || out.size() > MAX_TEMPLATE_BYTES - encoded.size()) return false;
          out += encoded;
          pos = end + 1;
          replaced = true;
        }
      }
    }
    if (!replaced) {
      const char c = tpl[pos++];
      if (context == TemplateContext::Json) {
        if (escaped)
          escaped = false;
        else if (quoted && c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = !quoted;
      }
      if (out.size() >= MAX_TEMPLATE_BYTES) return false;
      out += c;
    }
  }
  if (context == TemplateContext::Url && !validUrl(out)) return false;
  if (context == TemplateContext::Json && !out.empty() && !TemplateJsonValidator(out).validate()) return false;
  if (context == TemplateContext::Header)
    for (unsigned char c : out)
      if (c < 32 || c == 127) return false;
  return true;
}
}  // namespace pluginhttp
