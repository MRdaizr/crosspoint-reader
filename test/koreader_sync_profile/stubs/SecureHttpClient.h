#pragma once
#include <string>
namespace freeink {
class SecureHttpClient {
 public:
  inline static int nextCode = 200;
  inline static std::string nextResponse = "{}";
  inline static std::string lastUrl;
  inline static std::string lastBody;
  void setInsecure() {}
  bool begin(const std::string& url) {
    lastUrl = url;
    return true;
  }
  void addHeader(const std::string&, const std::string&) {}
  int GET() { return nextCode; }
  int sendRequest(const char*, const std::string& body) {
    lastBody = body;
    return nextCode;
  }
  std::string getString() { return nextResponse; }
  void end() {}
};
}  // namespace freeink
