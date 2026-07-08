#include "NutstoreWebDavClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <utility>

namespace {
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 2048;
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_TLS = 36000;
constexpr size_t MAX_ENUM_DIRS = 200;

std::string trimTrailingSlash(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

std::string ensureTrailingSlash(std::string s) {
  if (s.empty() || s.back() != '/') s.push_back('/');
  return s;
}

std::string urlOrigin(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  const size_t path = url.find('/', scheme + 3);
  return path == std::string::npos ? url : url.substr(0, path);
}

std::string urlPath(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "/";
  const size_t path = url.find('/', scheme + 3);
  return path == std::string::npos ? "/" : url.substr(path);
}

bool isUnreserved(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string percentEncodePath(const std::string& path) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (unsigned char c : path) {
    if (c == '/' || isUnreserved(static_cast<char>(c))) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size()) {
      const int hi = hexValue(in[i + 1]);
      const int lo = hexValue(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

std::string localName(const char* name) {
  const char* p = std::strrchr(name, '|');
  return p ? std::string(p + 1) : std::string(name);
}

struct DavParseState {
  std::vector<NutstoreRemoteEntry> entries;
  NutstoreRemoteEntry current;
  std::string currentText;
  std::string currentTag;
  bool inResponse = false;
  bool inProp = false;
  bool inResourceType = false;
};

void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* st = static_cast<DavParseState*>(userData);
  const std::string tag = localName(name);
  st->currentTag = tag;
  st->currentText.clear();
  if (tag == "response") {
    st->current = NutstoreRemoteEntry{};
    st->inResponse = true;
  } else if (tag == "prop") {
    st->inProp = true;
  } else if (tag == "resourcetype") {
    st->inResourceType = true;
  } else if (st->inResourceType && tag == "collection") {
    st->current.isDirectory = true;
  }
}

void XMLCALL endElement(void* userData, const XML_Char* name) {
  auto* st = static_cast<DavParseState*>(userData);
  const std::string tag = localName(name);
  if (st->inResponse) {
    if (tag == "href") {
      st->current.href = st->currentText;
    } else if (st->inProp && tag == "getcontentlength") {
      st->current.size = static_cast<size_t>(strtoull(st->currentText.c_str(), nullptr, 10));
    } else if (st->inProp && tag == "getlastmodified") {
      st->current.lastModified = st->currentText;
    }
  }
  if (tag == "response") {
    if (!st->current.href.empty()) st->entries.push_back(st->current);
    st->inResponse = false;
  } else if (tag == "prop") {
    st->inProp = false;
  } else if (tag == "resourcetype") {
    st->inResourceType = false;
  }
  st->currentText.clear();
  st->currentTag.clear();
}

void XMLCALL characterData(void* userData, const XML_Char* s, int len) {
  auto* st = static_cast<DavParseState*>(userData);
  if (!st->currentTag.empty()) st->currentText.append(s, len);
}

bool parseDavXml(const std::string& xml, std::vector<NutstoreRemoteEntry>& out, std::string& error) {
  XML_Parser parser = XML_ParserCreateNS(nullptr, '|');
  if (!parser) {
    error = "XML parser allocation failed";
    return false;
  }
  DavParseState state;
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  const bool ok = XML_Parse(parser, xml.c_str(), static_cast<int>(xml.size()), XML_TRUE) == XML_STATUS_OK;
  if (!ok) {
    error = "WebDAV XML parse failed";
    XML_ParserFree(parser);
    return false;
  }
  XML_ParserFree(parser);
  out = std::move(state.entries);
  return true;
}

void setBasicAuth(esp_http_client_handle_t client, const std::string& username, const std::string& password) {
  if (username.empty() || password.empty()) return;
  const std::string credentials = username + ":" + password;
  const String header = "Basic " + base64::encode(credentials.c_str());
  esp_http_client_set_header(client, "Authorization", header.c_str());
}

bool ensureParentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string parent = path.substr(0, slash);
  return Storage.mkdir(parent.c_str());
}
}  // namespace

NutstoreWebDavClient::NutstoreWebDavClient(std::string baseUrl, std::string username, std::string password)
    : baseUrl(ensureTrailingSlash(std::move(baseUrl))), username(std::move(username)), password(std::move(password)) {
  origin = urlOrigin(this->baseUrl);
  basePath = ensureTrailingSlash(urlPath(this->baseUrl));
}

std::string NutstoreWebDavClient::buildCollectionUrl(const std::string& remotePath) const {
  std::string path = remotePath.empty() ? "/" : remotePath;
  if (path[0] != '/') path = "/" + path;
  const std::string encoded = percentEncodePath(path);
  std::string url = trimTrailingSlash(baseUrl);
  if (encoded != "/") url += encoded;
  return ensureTrailingSlash(url);
}

std::string NutstoreWebDavClient::relativeFromHref(const std::string& href) const {
  std::string path = href;
  if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
    path = urlPath(path);
  }
  const std::string decoded = percentDecode(path);
  std::string base = percentDecode(rootPath);
  base = ensureTrailingSlash(base);
  if (decoded.rfind(base, 0) != 0) return "";
  std::string rel = decoded.substr(base.size());
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  while (!rel.empty() && rel.back() == '/') rel.pop_back();
  if (rel.find("..") != std::string::npos) return "";
  return rel;
}

bool NutstoreWebDavClient::propfindDepth1(const std::string& collectionUrl, std::vector<NutstoreRemoteEntry>& entries,
                                          std::string& error) {
  esp_http_client_config_t config = {};
  config.url = collectionUrl.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  LOG_DBG("NUT", "PROPFIND %s (heap: %u, max alloc: %u)", collectionUrl.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    error = "HTTP client allocation failed";
    return false;
  }
  esp_http_client_set_method(client, HTTP_METHOD_PROPFIND);
  esp_http_client_set_header(client, "Depth", "1");
  esp_http_client_set_header(client, "Content-Type", "application/xml; charset=utf-8");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  setBasicAuth(client, username, password);

  static constexpr const char* BODY =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>";
  esp_err_t err = esp_http_client_open(client, strlen(BODY));
  if (err != ESP_OK) {
    error = std::string("PROPFIND open failed: ") + esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_write(client, BODY, strlen(BODY));
  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 207 && status != 200) {
    error = "PROPFIND failed: HTTP " + std::to_string(status);
    esp_http_client_cleanup(client);
    return false;
  }

  std::string xml;
  char buf[READ_CHUNK];
  while (true) {
    const int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read < 0) {
      error = "PROPFIND read failed";
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) break;
    xml.append(buf, read);
  }
  esp_http_client_cleanup(client);

  std::vector<NutstoreRemoteEntry> parsed;
  if (!parseDavXml(xml, parsed, error)) return false;
  for (auto& e : parsed) {
    e.relativePath = relativeFromHref(e.href);
    if (!e.relativePath.empty()) entries.push_back(std::move(e));
  }
  return true;
}

bool NutstoreWebDavClient::listRecursive(const std::string& remotePath, std::vector<NutstoreRemoteEntry>& entries,
                                         std::string& error) {
  entries.clear();
  const std::string rootUrl = buildCollectionUrl(remotePath);
  rootPath = urlPath(rootUrl);

  std::vector<std::string> queue = {rootUrl};
  std::set<std::string> seenDirs;
  size_t scannedDirs = 0;
  while (!queue.empty()) {
    const std::string url = queue.back();
    queue.pop_back();
    if (!seenDirs.insert(url).second) continue;
    scannedDirs++;
    if (scannedDirs > MAX_ENUM_DIRS) {
      error = "Too many folders. Set smaller Remote Path.";
      return false;
    }

    if (ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_TLS) {
      error = "Low TLS memory. Set smaller Remote Path.";
      return false;
    }

    std::vector<NutstoreRemoteEntry> listed;
    if (!propfindDepth1(url, listed, error)) return false;
    for (const auto& e : listed) {
      if (e.isDirectory) {
        std::string child = e.href.rfind("http", 0) == 0 ? e.href : origin + e.href;
        child = ensureTrailingSlash(child);
        queue.push_back(child);
      } else {
        entries.push_back(e);
      }
    }
  }
  return true;
}

bool NutstoreWebDavClient::downloadFile(const NutstoreRemoteEntry& entry, const std::string& destPath,
                                        ProgressCallback progress, std::string& error) {
  const std::string url = entry.href.rfind("http", 0) == 0 ? entry.href : origin + entry.href;
  const std::string tmpPath = destPath + ".tmp";
  ensureParentDir(destPath);
  Storage.remove(tmpPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("NUT", tmpPath.c_str(), file)) {
    error = "Could not create local file";
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  LOG_DBG("NUT", "GET %s (heap: %u, max alloc: %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    file.close();
    Storage.remove(tmpPath.c_str());
    error = "HTTP client allocation failed";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  setBasicAuth(client, username, password);

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    file.close();
    Storage.remove(tmpPath.c_str());
    esp_http_client_cleanup(client);
    error = std::string("Download open failed: ") + esp_err_to_name(err);
    return false;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    file.close();
    Storage.remove(tmpPath.c_str());
    esp_http_client_cleanup(client);
    error = "Download failed: HTTP " + std::to_string(status);
    return false;
  }

  char buf[READ_CHUNK];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? static_cast<size_t>(contentLength) : entry.size;
  while (true) {
    const int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read < 0) {
      file.close();
      Storage.remove(tmpPath.c_str());
      esp_http_client_cleanup(client);
      error = "Download read failed";
      return false;
    }
    if (read == 0) break;
    if (file.write(buf, read) != static_cast<size_t>(read)) {
      file.close();
      Storage.remove(tmpPath.c_str());
      esp_http_client_cleanup(client);
      error = "SD write failed";
      return false;
    }
    downloaded += read;
    if (progress && total > 0) progress(downloaded, total);
  }
  file.close();
  esp_http_client_cleanup(client);

  if (downloaded == 0 && entry.size > 0) {
    Storage.remove(tmpPath.c_str());
    error = "Downloaded file is empty";
    return false;
  }
  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    error = "Could not finalize downloaded file";
    return false;
  }
  return true;
}
