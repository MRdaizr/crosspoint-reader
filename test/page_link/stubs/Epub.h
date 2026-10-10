#pragma once
#include <string>
class Epub {
 public:
  template <typename Stream>
  bool readItemContentsToStream(const std::string&, Stream&, size_t, bool = false) {
    return false;
  }
};
