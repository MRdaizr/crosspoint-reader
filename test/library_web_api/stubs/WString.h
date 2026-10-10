#pragma once
#include <string>
#include <utility>

// Byte lengths and NUL termination intentionally match the Arduino boundaries
// used by the production handlers; no HTTP/filtering logic lives in this stub.
class String {
  std::string value;

 public:
  String() = default;
  String(const char* text) : value(text ? text : "") {}
  String(std::string text) : value(std::move(text)) {}
  const char* c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
  bool isEmpty() const { return value.empty(); }
  char operator[](size_t i) const { return value[i]; }
  bool operator==(const char* other) const { return value == other; }
  bool operator!=(const char* other) const { return value != other; }
};
