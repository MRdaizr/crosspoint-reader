#pragma once
#include <string>
using String = std::string;
namespace base64 {
inline String encode(const char*) { return "encoded-host-credentials"; }
}  // namespace base64
