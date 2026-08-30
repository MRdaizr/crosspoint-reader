#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <string>
#include <utility>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// wolfSSL has a smaller TLS footprint than the previous mbedTLS path, but a
// reading session can still leave too little contiguous heap for a handshake.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

bool beginClient(freeink::SecureHttpClient& http, const std::string& url) {
  // Preserve the existing trusted-network behavior while moving the transport
  // to FreeInk's wolfSSL client. Protocols and auth headers are unchanged.
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return false;
  }
  applyAuthHeaders(http);
  return true;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginClient(http, url)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return NETWORK_ERROR;
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginClient(http, url)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (httpCode == 204) {
    http.end();
    return NOT_FOUND;
  }

  if (httpCode >= 200 && httpCode < 300) {
    const auto response = http.getString();
    http.end();
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, response.c_str());
    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.metadata.reset();
    if (KOREADER_STORE.getSendMetadata()) {
      const JsonObjectConst metadata = doc["metadata"].as<JsonObjectConst>();
      if (!metadata.isNull()) {
        KOReaderMetadata value;
        value.filename = metadata["filename"].as<const char*>() ? metadata["filename"].as<const char*>() : "";
        value.title = metadata["title"].as<const char*>() ? metadata["title"].as<const char*>() : "";
        value.authors = metadata["authors"].as<const char*>() ? metadata["authors"].as<const char*>() : "";
        outProgress.metadata = std::move(value);
      }
    }

    // The rich position is a CrossPoint extension and must never be sent to
    // or trusted from arbitrary KOSync-compatible servers.
    outProgress.position.reset();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      const JsonObjectConst position = doc["position"].as<JsonObjectConst>();
      if (!position.isNull()) {
        KOReaderRichPosition value;
        value.pctQ = position["pctQ"].as<uint32_t>();
        value.spineIndex = position["spine"].as<uint16_t>();
        value.pageNumber = position["page"].as<uint16_t>();
        const uint16_t pages = position["pages"].as<uint16_t>();
        value.totalPages = pages > 0 ? pages : 1;
        const uint16_t paragraph = position["para"].as<uint16_t>();
        if (paragraph > 0) value.paragraphIndex = paragraph;
        const char* xpath = position["xpath"].as<const char*>();
        if (xpath) value.xpath = xpath;
        outProgress.position = std::move(value);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  if (progress.metadata.has_value()) {
    auto metadata = doc["metadata"].to<JsonObject>();
    metadata["filename"] = progress.metadata->filename;
    metadata["title"] = progress.metadata->title;
    metadata["authors"] = progress.metadata->authors;
  }

  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    const auto& position = *progress.position;
    auto rich = doc["position"].to<JsonObject>();
    rich["pctQ"] = position.pctQ;
    rich["spine"] = position.spineIndex;
    rich["page"] = position.pageNumber;
    rich["pages"] = position.totalPages > 0 ? position.totalPages : 1;
    if (position.paragraphIndex.has_value()) rich["para"] = *position.paragraphIndex;
    // Keep the server-side validation limit in the client as well.  Oversized
    // XPath strings are omitted rather than rejecting an otherwise valid sync.
    if (!position.xpath.empty() && position.xpath.size() <= 120) rich["xpath"] = position.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  if (!beginClient(http, url)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    case USER_EXISTS:
      return "Username already exists";
    default:
      return "Unknown error";
  }
}
