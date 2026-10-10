#pragma once
#include <string>
class MD5Builder {
 public:
  void begin() {}
  void add(const char*) {}
  void calculate() {}
  std::string toString() const { return "host-md5"; }
};
