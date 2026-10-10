#pragma once
#include <string>
namespace obfuscation {
// Device MAC/base64 are outside these protocol/migration tests.
inline std::string obfuscateToBase64(const std::string& value) { return value; }
}  // namespace obfuscation
