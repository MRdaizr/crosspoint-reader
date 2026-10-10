#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "WString.h"

enum HTTPMethod { HTTP_GET, HTTP_POST, HTTP_PUT };
inline constexpr size_t CONTENT_LENGTH_UNKNOWN = static_cast<size_t>(-1);

class FakeClient {
 public:
  bool live = true;
  bool stopped = false;
  bool connected() const { return live; }
  void stop() {
    live = false;
    stopped = true;
  }
};

class WebServer {
 public:
  using Arguments = std::map<std::string, std::string>;
  std::map<std::pair<std::string, HTTPMethod>, std::function<void()>> routes;
  Arguments arguments;
  HTTPMethod requestMethod = HTTP_GET;
  FakeClient connection;
  int status = 0;
  std::vector<int> responseCodes;
  std::string contentType, body;
  std::map<std::string, std::string> headers;
  std::vector<std::string> chunks;
  size_t contentLength = 0;
  bool terminated = false;
  void resetResponse() {
    status = 0;
    responseCodes.clear();
    contentType.clear();
    body.clear();
    headers.clear();
    chunks.clear();
    contentLength = 0;
    terminated = false;
  }
  HTTPMethod method() const { return requestMethod; }
  bool hasArg(const char* name) const { return arguments.contains(name); }
  String arg(const char* name) const {
    const auto found = arguments.find(name);
    return found == arguments.end() ? String() : String(found->second);
  }
  FakeClient& client() { return connection; }
  void send(int code, const char* type, const char* text) {
    status = code;
    responseCodes.push_back(code);
    contentType = type;
    body = text;
  }
  void sendHeader(const char* name, const char* value) { headers[name] = value; }
  void setContentLength(size_t length) { contentLength = length; }
  void sendContent(const char* data, size_t length) {
    chunks.emplace_back(data, length);
    body.append(data, length);
    if (!length) terminated = true;
  }
  void on(const char* url, HTTPMethod method, std::function<void()> callback) {
    routes[{url, method}] = std::move(callback);
  }
  bool request(const std::string& url, HTTPMethod method, const Arguments& args = {}) {
    resetResponse();
    requestMethod = method;
    arguments = args;
    const auto found = routes.find({url, method});
    if (found == routes.end()) return false;
    found->second();
    return true;
  }
};
