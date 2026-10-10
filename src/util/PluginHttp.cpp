#include "PluginHttp.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PluginHttpPolicy.h"
#include "network/DeadlineDns.h"
#include "network/ProtectedPaths.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr size_t MAX_TOKEN_FILE_SIZE = 2 * 1024;
struct PowerGuard {
  wifi_ps_type_t previous = WIFI_PS_MIN_MODEM;
  bool restore = false;
  PowerGuard() {
    restore = esp_wifi_get_ps(&previous) == ESP_OK;
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  ~PowerGuard() {
    if (restore) esp_wifi_set_ps(previous);
  }
};
}  // namespace
namespace pluginhttp {

// True when a dotted-path segment addresses an array index.
static bool segIsIndex(const std::string& seg) { return !seg.empty() && isdigit(static_cast<unsigned char>(seg[0])); }

void splitPath(const std::string& dotted, std::vector<std::string>& out) {
  out.clear();
  out.reserve(static_cast<size_t>(std::count(dotted.begin(), dotted.end(), '.')) + 1);
  size_t start = 0;
  while (start <= dotted.size()) {
    const size_t dot = dotted.find('.', start);
    if (dot == std::string::npos) {
      if (start < dotted.size()) out.push_back(dotted.substr(start));
      break;
    }
    if (dot > start) out.push_back(dotted.substr(start, dot - start));
    start = dot + 1;
  }
}

JsonVariantConst resolvePath(JsonVariantConst node, const std::string& dotted) {
  if (dotted.empty()) return node;
  std::vector<std::string> segs;
  splitPath(dotted, segs);
  for (const auto& seg : segs) {
    if (node.isNull()) break;
    if (segIsIndex(seg)) {
      node = node[atoi(seg.c_str())];
    } else {
      node = node[seg.c_str()];
    }
  }
  return node;
}

std::string variantToString(JsonVariantConst v) {
  if (v.isNull()) return "";
  if (v.is<const char*>()) return v.as<const char*>();
  char buf[24];
  if (v.is<long long>() || v.is<int>()) {
    snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
    return buf;
  }
  return "";
}

void substituteAll(std::string& s, const char* key, const std::string& value) {
  const size_t keyLen = strlen(key);
  if (!keyLen || s.size() > 8192) {
    s.clear();
    return;
  }
  size_t pos = 0;
  while ((pos = s.find(key, pos)) != std::string::npos) {
    if (value.size() > 8192 || s.size() - keyLen > 8192 - value.size()) {
      s.clear();
      return;
    }
    s.replace(pos, keyLen, value);
    pos += value.size();
  }
}

void readRequest(JsonVariantConst node, const char* defaultMethod, RequestSpec& out) {
  out.url = node["url"] | "";
  out.method = node["method"] | defaultMethod;
  out.body = node["body"] | "";
  readHeaders(node["headers"], out.headers);
}

void readHeaders(JsonVariantConst node, Headers& out) {
  out.clear();
  const JsonObjectConst headers = node.as<JsonObjectConst>();
  out.reserve(std::min<size_t>(headers.size(), 16));
  for (JsonPairConst kv : headers) {
    if (out.size() >= 16) break;
    out.emplace_back(kv.key().c_str(), kv.value().as<const char*>() ? kv.value().as<const char*>() : "");
  }
}

std::string urlEncodeQuery(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string inPluginDir(const std::string& pluginDir, const char* path) {
  if (!path || !path[0] || path[0] == '/') return path ? path : "";
  return pluginDir + "/" + path;
}

// A manifest may not point the firmware at a credential store.
static bool allowedFile(const std::string& file) {
  if (protectedpaths::isPluginPath(file)) return true;
  LOG_ERR("PHTP", "Manifest file not allowed: %s", file.c_str());
  return false;
}

bool loadTokenFromFile(const std::string& file, const std::string& path, std::string& out) {
  out.clear();
  if (file.empty()) return true;  // token-less plugin
  if (!allowedFile(file)) return false;
  std::string raw;
  if (!readFile(file, MAX_TOKEN_FILE_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  out = variantToString(resolvePath(doc.as<JsonVariantConst>(), path));
  return !out.empty();
}

bool saveTokenToFile(const std::string& file, const std::string& path, const std::string& value) {
  if (file.empty()) return false;
  if (!allowedFile(file)) return false;
  JsonDocument doc;
  // Build the nesting the read path expects (numeric segments unsupported).
  std::vector<std::string> segs;
  splitPath(path, segs);
  if (segs.empty()) return false;
  JsonVariant node = doc.to<JsonObject>();
  for (size_t i = 0; i + 1 < segs.size(); i++) node = node[segs[i]].to<JsonObject>();
  node[segs.back()] = value;
  std::string out;
  serializeJson(doc, out);
  if (out.size() > MAX_TOKEN_FILE_SIZE) return false;
  const std::string tmp = file + ".tmp";
  return protectedpaths::isPluginPath(tmp) && protectedpaths::isPluginPath(file + ".bak") &&
         writeStaged(tmp, out.data(), out.size()) && replaceFile(tmp, file);
}

void loadConfigFile(const std::string& file, Headers& out) {
  out.clear();
  if (file.empty()) return;
  if (!allowedFile(file)) return;
  std::string raw;
  if (!readFile(file, MAX_TOKEN_FILE_SIZE, raw)) return;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  const JsonObjectConst fields = doc.as<JsonObjectConst>();
  out.reserve(std::min<size_t>(fields.size(), 24));
  for (JsonPairConst kv : fields) {
    if (out.size() >= 24) break;
    out.emplace_back(kv.key().c_str(), variantToString(kv.value()));
  }
}

bool readFile(const std::string& path, size_t cap, std::string& out) {
  return Storage.readFileToString("PLG", path, cap, out);
}
bool writeStaged(const std::string& tmp, const void* data, size_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("PLG", tmp, file)) return false;
  const bool ok = file.write(data, size) == size;
  file.flush();
  // Publication callers require both durable data and a successfully released
  // writer; a destructor-only close would hide SD sync/close failures.
  const bool closed = file.close();
  if (!ok || !closed) {
    Storage.remove(tmp.c_str());
    LOG_ERR("PLG", "staged write/close failed");
  }
  return ok && closed;
}
bool replaceFile(const std::string& tmp, const std::string& path) {
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

// No auto-redirects: OAuth requests may contain secrets in headers or bodies.
// A 3xx is returned to the caller. File GET redirects are handled separately
// by HttpDownloader, which strips credentials on a change of origin.
static int transport(freeink::SecureHttpClient* reused, const std::string& url, const std::string& method,
                     const std::string& body, const Headers& headers,
                     const freeink::SecureHttpClient::DataCallback& sink, Headers* responseHeaders,
                     const std::function<bool()>& shouldAbort, uint32_t deadlineMs) {
  if (!validUrl(url) || body.size() > 8192 || headers.size() > 16 ||
      (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE" && method != "PROPFIND" &&
       method != "HEAD" && method != "PATCH"))
    return -1;
  for (const auto& h : headers)
    if (!validHeader(h.first, h.second)) return -1;
  for (unsigned char c : body)
    if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c == 127) return -1;
  if (deadlineMs == 0) deadlineMs = millis() + 30000;
  DeadlineDns::Guard dnsDeadline(
      deadlineMs,
      [](void* ctx) {
        const auto& cancel = *static_cast<const std::function<bool()>*>(ctx);
        return cancel && cancel();
      },
      const_cast<std::function<bool()>*>(&shouldAbort));
  freeink::SecureHttpClient* checkingClient = nullptr;
  bool bodyStarted = false;
  const auto abort = [&] {
    resetTaskWatchdogIfSubscribed();
    if (static_cast<int32_t>(millis() - deadlineMs) >= 0 || (shouldAbort && shouldAbort())) return true;
    // The current SDK checks this between header lines. Stop its otherwise
    // unbounded header vector before reading another line; at most one SDK-
    // bounded line can exceed our 4KB budget. Skip copies once streaming starts.
    return checkingClient && !bodyStarted && checkingClient->getStatus() > 0 &&
           !responseHeadersWithinBudget(checkingClient->getHeaders());
  };
  if (abort()) return -1;
  // The client contains vectors/strings and TLS state: a single checked heap
  // allocation avoids a large client object on the ESP task stack.
  auto owned = reused ? std::unique_ptr<freeink::SecureHttpClient>{} : makeUniqueNoThrow<freeink::SecureHttpClient>();
  auto* http = reused ? reused : owned.get();
  if (!http) {
    LOG_ERR("PHTP", "OOM: HTTP client");
    return -1;
  }
  PowerGuard power;
  http->setUserAgent("CrossPoint");
  http->setInsecure();  // Match the current SDK consumers (no CA bundle).
  http->clearBasicAuth();
  http->setFollowRedirects(0);
  http->setAllowRedirectDowngrade(false);
  // Current SecureClient can try TCP+TLS twice. Give each blocking phase at
  // most a quarter of the remaining budget; reads still check absolute time.
  const auto timeout = phaseTimeout(millis(), deadlineMs);
  if (!timeout) return -1;
  http->setTimeout(timeout);
  if (!http->begin(url)) return -1;
  for (const auto& h : headers) http->addHeader(h.first, h.second);
  checkingClient = http;
  const int status = http->sendRequest(
      method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, size_t length) {
        bodyStarted = true;
        return sink(data, length);
      },
      abort);
  if (responseHeaders) {
    *responseHeaders = http->getHeaders();
    size_t bytes = 0;
    for (const auto& h : *responseHeaders) bytes += h.first.size() + h.second.size();
    if (responseHeaders->size() > 32 || bytes > 4096) return -1;
  }
  return status >= 0 && http->responseComplete() && !abort() ? status : -1;
}
int request(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
            const std::string& body, const Headers& headers, String& out, size_t maxResponse, Headers* responseHeaders,
            const std::function<bool()>& shouldAbort, uint32_t deadlineMs) {
  out.remove(0);
  if (!maxResponse || maxResponse > 48 * 1024 || !out.reserve(maxResponse)) {
    LOG_ERR("PHTP", "OOM/invalid response cap");
    return -1;
  }
  return transport(
      session, url, method, body, headers,
      [&](const uint8_t* data, size_t len) {
        return len <= maxResponse - out.length() && out.concat(reinterpret_cast<const char*>(data), len);
      },
      responseHeaders, shouldAbort, deadlineMs);
}
int requestToFile(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                  const std::string& body, const Headers& headers, const char* destPath, size_t maxResponse,
                  const std::function<bool()>& shouldAbort, uint32_t deadlineMs) {
  if (!destPath || !protectedpaths::isPluginPath(destPath) || !maxResponse || maxResponse > 32 * 1024 * 1024) return -1;
  size_t written = 0;
  int status;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PHTP", destPath, file)) return -1;
    status = transport(
        session, url, method, body, headers,
        [&](const uint8_t* data, size_t len) {
          if (len > maxResponse - written || file.write(data, len) != len) return false;
          written += len;
          return true;
        },
        nullptr, shouldAbort, deadlineMs);
    file.flush();
    if (!file.close()) status = -1;
  }
  if (status < 0) Storage.remove(destPath);
  return status;
}
bool mintPasswordToken(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                       const std::string& body, const Headers& headers, const std::string& tokenPath,
                       std::string& outToken, uint32_t deadlineMs, const std::function<bool()>& shouldAbort) {
  String response;
  const int status = request(session, url, method, body, headers, response, 8 * 1024, nullptr, shouldAbort, deadlineMs);
  if (status < 200 || status >= 300) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) return false;
  outToken = variantToString(resolvePath(doc.as<JsonVariantConst>(), tokenPath));
  return !outToken.empty() && outToken.size() <= 1024;
}
}  // namespace pluginhttp
