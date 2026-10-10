#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_wifi.h>

#include <functional>
#include <optional>
#include <string>

#include "DeadlineDns.h"
#include "util/PluginHttpPolicy.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t maxBytes = 0;
  uint32_t deadlineMs = 0;
  std::function<bool()> shouldAbort;
  bool stopped() const {
    return (cancelFlag && *cancelFlag) || (shouldAbort && shouldAbort()) ||
           (deadlineMs && static_cast<int32_t>(millis() - deadlineMs) >= 0);
  }
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// OtaUpdater.cpp already disables WiFi power-save for firmware downloads, but
// OPDS feed/book fetches never did despite being able to run just as long for
// a large category. Modem sleep periodically powers the radio down between
// DTIM beacon intervals, which can drop or stall packets mid-transfer -- more
// likely to be hit the longer a transfer takes, so small feeds mostly get
// away with it while a large category consistently doesn't.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to restore WiFi power-save: %d", err);
  }
};

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password,
                                         const std::vector<HttpDownloader::Header>& headers, Sink& sink) {
  // Normal OPDS/firmware transfers keep the SDK's original DNS behavior.
  std::optional<DeadlineDns::Guard> dnsDeadline;
  if (sink.deadlineMs)
    dnsDeadline.emplace(sink.deadlineMs, [](void* ctx) { return static_cast<Sink*>(ctx)->stopped(); }, &sink);
  WifiPowerSaveGuard psGuard;
  std::string url = startUrl;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    auto client = makeUniqueNoThrow<freeink::SecureHttpClient>();
    if (!client) {
      LOG_ERR("HTTP", "OOM: client");
      return HttpDownloader::HTTP_ERROR;
    }
    auto& http = *client;
    if (sink.stopped()) return HttpDownloader::ABORTED;
    if (!pluginhttp::validUrl(url)) return HttpDownloader::HTTP_ERROR;
    const auto timeout = pluginhttp::phaseTimeout(millis(), sink.deadlineMs, HTTP_TIMEOUT_MS);
    if (!timeout) return HttpDownloader::ABORTED;
    http.setTimeout(timeout);
    // Existing CrossPoint endpoints include local OPDS servers and the
    // WeRead/KOSync trusted-network services. Preserve their historical
    // transport behavior while moving the TLS implementation to FreeInk.
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (pluginhttp::sameOrigin(startUrl, url) && !username.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    if (pluginhttp::sameOrigin(startUrl, url))
      for (const auto& header : headers) {
        if (!pluginhttp::validHeader(header.first, header.second)) return HttpDownloader::HTTP_ERROR;
        http.addHeader(header.first, header.second);
      }
    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    bool bodyStarted = false;
    const bool boundedPluginTransfer = sink.maxBytes || sink.deadlineMs || !headers.empty();
    const int status = http.GET(
        [&http, &sink, &bodyStarted](const uint8_t* data, size_t len) {
          bodyStarted = true;
          if (http.getStatus() != 200) return true;
          if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
          if (sink.stopped() || (sink.maxBytes && len > sink.maxBytes - sink.downloaded) || !sink.write(data, len))
            return false;
          sink.downloaded += len;
          if (sink.progress) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink, &http, &bodyStarted, boundedPluginTransfer]() {
          return sink.stopped() || (boundedPluginTransfer && !bodyStarted && http.getStatus() > 0 &&
                                    !pluginhttp::responseHeadersWithinBudget(http.getHeaders()));
        });

    if (http.aborted() || sink.stopped()) return HttpDownloader::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !pluginhttp::resolveRedirect(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      continue;
    }
    if (status == 401 || status == 403) return HttpDownloader::UNAUTHORIZED;
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
#if !defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetEsp(const std::string& startUrl, const std::string& username,
                                        const std::string& password, const std::vector<HttpDownloader::Header>& headers,
                                        Sink& sink) {
  WifiPowerSaveGuard psGuard;
  std::string url = startUrl;
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: read buffer");
    return HttpDownloader::HTTP_ERROR;
  }
  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    if (sink.stopped()) return HttpDownloader::ABORTED;
    if (!pluginhttp::validUrl(url)) return HttpDownloader::HTTP_ERROR;
    std::string location;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = pluginhttp::phaseTimeout(millis(), sink.deadlineMs, HTTP_TIMEOUT_MS);
    if (!config.timeout_ms) return HttpDownloader::ABORTED;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.user_data = &location;
    config.event_handler = [](esp_http_client_event_t* event) -> esp_err_t {
      if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
          strcasecmp(event->header_key, "Location") == 0 && event->header_value && strlen(event->header_value) <= 2048)
        *static_cast<std::string*>(event->user_data) = event->header_value;
      return ESP_OK;
    };
    auto client = esp_http_client_init(&config);
    if (!client) return HttpDownloader::HTTP_ERROR;
    esp_http_client_set_header(client, "User-Agent", "CrossPoint");
    if (pluginhttp::sameOrigin(startUrl, url)) {
      if (!username.empty()) {
        const auto credentials = username + ":" + password;
        const String basic = "Basic " + base64::encode(credentials.c_str());
        esp_http_client_set_header(client, "Authorization", basic.c_str());
      }
      for (const auto& h : headers) {
        if (!pluginhttp::validHeader(h.first, h.second)) {
          esp_http_client_cleanup(client);
          return HttpDownloader::HTTP_ERROR;
        }
        esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());
      }
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    const int64_t length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (isRedirect(status)) {
      esp_http_client_cleanup(client);
      if (location.empty() || !pluginhttp::resolveRedirect(url, location, url)) return HttpDownloader::HTTP_ERROR;
      continue;
    }
    if (status != 200) {
      esp_http_client_cleanup(client);
      return status == 401 || status == 403 ? HttpDownloader::UNAUTHORIZED : HttpDownloader::HTTP_ERROR;
    }
    sink.total = length > 0 ? length : 0;
    while (true) {
      if (sink.stopped()) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }
      const int n = esp_http_client_read(client, buf.get(), READ_CHUNK);
      if (n < 0) {
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (n == 0) break;
      if ((sink.maxBytes && static_cast<size_t>(n) > sink.maxBytes - sink.downloaded) ||
          !sink.write(reinterpret_cast<const uint8_t*>(buf.get()), n)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }
      sink.downloaded += n;
      if (sink.progress) sink.progress(sink.downloaded, sink.total);
    }
    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    return complete ? HttpDownloader::OK : HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::HTTP_ERROR;
}
#endif

HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, username, password, headers, sink);
#else
  return runGetEsp(url, username, password, headers, sink);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, const std::vector<Header>& headers) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, headers, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, const std::vector<Header>& headers) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, headers, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, const std::vector<Header>& headers) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, headers, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers, size_t maxBytes,
                                                             uint32_t deadlineMs,
                                                             const std::function<bool()>& shouldAbort) {
  const std::string tmp = destPath + ".part";
  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.maxBytes = maxBytes;
  sink.deadlineMs = deadlineMs;
  sink.shouldAbort = shouldAbort;
  DownloadError result;
  {
    HalFile file;
    if (!Storage.openFileForWrite("HTTP", tmp, file)) return FILE_ERROR;
    sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
    result = runGetSecure(url, username, password, headers, sink);
    file.flush();
    if (!file.close()) result = FILE_ERROR;
  }
  if (result != OK || !sink.downloaded || sink.stopped()) {
    Storage.remove(tmp.c_str());
    return sink.stopped() ? ABORTED : (result == OK ? HTTP_ERROR : result);
  }
  if (!Storage.replaceFile(tmp.c_str(), destPath.c_str())) {
    Storage.remove(tmp.c_str());
    return FILE_ERROR;
  }
  return OK;
}
