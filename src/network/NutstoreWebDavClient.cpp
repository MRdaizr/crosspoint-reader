#include "NutstoreWebDavClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace {
// Nutstore currently serves dav.jianguoyun.com through Sectigo's USERTrust RSA
// chain. Keeping the root here lets the wolfSSL path verify the peer without
// loading the much larger ESP-IDF mbedTLS certificate bundle during handshake.
static constexpr char NUTSTORE_ROOT_CA[] = R"pem(-----BEGIN CERTIFICATE-----
MIIF3jCCA8agAwIBAgIQAf1tMPyjylGoG7xkDjUDLTANBgkqhkiG9w0BAQwFADCB
iDELMAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0pl
cnNleSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNV
BAMTJVVTRVJUcnVzdCBSU0EgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAw
MjAxMDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNV
BAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVU
aGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBSU0EgQ2Vy
dGlmaWNhdGlvbiBBdXRob3JpdHkwggIiMA0GCSqGSIb3DQEBAQUAA4ICDwAwggIK
AoICAQCAEmUXNg7D2wiz0KxXDXbtzSfTTK1Qg2HiqiBNCS1kCdzOiZ/MPans9s/B
3PHTsdZ7NygRK0faOca8Ohm0X6a9fZ2jY0K2dvKpOyuR+OJv0OwWIJAJPuLodMkY
tJHUYmTbf6MG8YgYapAiPLz+E/CHFHv25B+O1ORRxhFnRghRy4YUVD+8M/5+bJz/
Fp0YvVGONaanZshyZ9shZrHUm3gDwFA66Mzw3LyeTP6vBZY1H1dat//O+T23LLb2
VN3I5xI6Ta5MirdcmrS3ID3KfyI0rn47aGYBROcBTkZTmzNg95S+UzeQc0PzMsNT
79uq/nROacdrjGCT3sTHDN/hMq7MkztReJVni+49Vv4M0GkPGw/zJSZrM233bkf6
c0Plfg6lZrEpfDKEY1WJxA3Bk1QwGROs0303p+tdOmw1XNtB1xLaqUkL39iAigmT
Yo61Zs8liM2EuLE/pDkP2QKe6xJMlXzzawWpXhaDzLhn4ugTncxbgtNMs+1b/97l
c6wjOy0AvzVVdAlJ2ElYGn+SNuZRkg7zJn0cTRe8yexDJtC/QV9AqURE9JnnV4ee
UB9XVKg+/XRjL7FQZQnmWEIuQxpMtPAlR1n6BB6T1CZGSlCBst6+eLf8ZxXhyVeE
Hg9j1uliutZfVS7qXMYoCAQlObgOK6nyTJccBz8NUvXt7y+CDwIDAQABo0IwQDAd
BgNVHQ4EFgQUU3m/WqorSs9UgOHYm8Cd8rIDZsswDgYDVR0PAQH/BAQDAgEGMA8G
A1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEMBQADggIBAFzUfA3P9wF9QZllDHPF
Up/L+M+ZBn8b2kMVn54CVVeWFPFSPCeHlCjtHzoBN6J2/FNQwISbxmtOuowhT6KO
VWKR82kV2LyI48SqC/3vqOlLVSoGIG1VeCkZ7l8wXEskEVX/JJpuXior7gtNn3/3
ATiUFJVDBwn7YKnuHKsSjKCaXqeYalltiz8I+8jRRa8YFWSQEg9zKC7F4iRO/Fjs
8PRF/iKz6y+O0tlFYQXBl2+odnKPi4w2r78NBc5xjeambx9spnFixdjQg3IM8WcR
iQycE0xyNN+81XHfqnHd4blsjDwSXWXavVcStkNr/+XeTWYRUc+ZruwXtuhxkYze
Sf7dNXGiFSeUHM9h4ya7b6NnJSFd5t0dCy5oGzuCr+yDZ4XUmFF0sbmZgIn/f3gZ
XHlKYC6SQK5MNyosycdiyA5d9zZbyuAlJQG03RoHnHcAP9Dc1ew91Pq7P8yF1m9/
qS3fuQL39ZeatTXaw2ewh0qpKJ4jjv9cJ2vhsE/zB+4ALtRZh8tSQZXq9EfX7mRB
VXyNWQKV3WKdwrnuWih0hKWbt5DHDAff9Yk2dDLWKMGwsAvgnEzDHNb842m1R0aB
L6KCq9NjRHDEjf8tM7qtj3u1cIiuPhnPQCjY/MiQu12ZIvVS5ljFH4gxQ+6IHdfG
jjxDah2nGN59PRbxYvnKkKj9
-----END CERTIFICATE-----
)pem";

constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr uint32_t MIN_MAX_ALLOC_FOR_TLS = 36000;
constexpr size_t MAX_ENUM_DIRS = 200;
constexpr int MAX_INCOMPLETE_RETRIES = 1;
constexpr int INCOMPLETE_RETRY_DELAY_MS = 250;
constexpr const char* DAV_STAGE_PATH = "/Nutstore/.nutstore-propfind-stage.tmp";

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

std::string relativePathFromHref(const std::string& href, const std::string& rootPath) {
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

bool writeStageValue(HalFile& file, const void* value, const size_t length) {
  return file.write(value, length) == length;
}

bool writeStageEntry(HalFile& file, const NutstoreRemoteEntry& entry) {
  if (entry.href.size() > UINT32_MAX || entry.relativePath.size() > UINT32_MAX) return false;
  const uint32_t hrefLength = static_cast<uint32_t>(entry.href.size());
  const uint32_t relativePathLength = static_cast<uint32_t>(entry.relativePath.size());
  const uint32_t size = static_cast<uint32_t>(entry.size);
  const uint8_t isDirectory = entry.isDirectory ? 1 : 0;
  return writeStageValue(file, &hrefLength, sizeof(hrefLength)) &&
         writeStageValue(file, &relativePathLength, sizeof(relativePathLength)) &&
         writeStageValue(file, &size, sizeof(size)) && writeStageValue(file, &isDirectory, sizeof(isDirectory)) &&
         writeStageValue(file, entry.href.data(), hrefLength) &&
         writeStageValue(file, entry.relativePath.data(), relativePathLength);
}

bool readStageBytes(HalFile& file, void* buffer, size_t length) {
  auto* bytes = static_cast<uint8_t*>(buffer);
  size_t offset = 0;
  while (offset < length) {
    const int count = file.read(bytes + offset, length - offset);
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

bool readStageEntry(HalFile& file, NutstoreRemoteEntry& entry) {
  uint32_t hrefLength = 0;
  uint32_t relativePathLength = 0;
  uint32_t size = 0;
  uint8_t isDirectory = 0;
  if (!readStageBytes(file, &hrefLength, sizeof(hrefLength)) ||
      !readStageBytes(file, &relativePathLength, sizeof(relativePathLength)) ||
      !readStageBytes(file, &size, sizeof(size)) || !readStageBytes(file, &isDirectory, sizeof(isDirectory))) {
    return false;
  }
  const uint64_t remaining = file.fileSize64() - file.position();
  if (hrefLength == 0 || relativePathLength == 0 || isDirectory > 1 ||
      static_cast<uint64_t>(hrefLength) + relativePathLength > remaining) {
    return false;
  }
  entry = {};
  entry.href.resize(hrefLength);
  entry.relativePath.resize(relativePathLength);
  entry.size = size;
  entry.isDirectory = isDirectory != 0;
  return readStageBytes(file, entry.href.data(), hrefLength) &&
         readStageBytes(file, entry.relativePath.data(), relativePathLength);
}

struct DavParseState {
  HalFile* stageFile = nullptr;
  const std::string* rootPath = nullptr;
  NutstoreRemoteEntry current;
  std::string currentText;
  std::string currentTag;
  std::string error;
  bool inResponse = false;
  bool inProp = false;
  bool inResourceType = false;
};

void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**);
void XMLCALL endElement(void* userData, const XML_Char* name);
void XMLCALL characterData(void* userData, const XML_Char* s, int len);

struct DavStream {
  XML_Parser parser = nullptr;
  DavParseState state;
  std::string error;

  ~DavStream() {
    if (parser) XML_ParserFree(parser);
  }

  bool begin(HalFile& stageFile, const std::string& rootPath) {
    if (parser) XML_ParserFree(parser);
    parser = XML_ParserCreateNS(nullptr, '|');
    state = DavParseState{};
    state.stageFile = &stageFile;
    state.rootPath = &rootPath;
    error.clear();
    if (!parser) {
      error = "XML parser allocation failed";
      LOG_ERR("NUT", "WebDAV XML parser allocation failed (heap: %u, max alloc: %u)",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return false;
    }
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterData);
    return true;
  }

  bool feed(const char* data, const int length, const bool finished = false) {
    if (!parser || XML_Parse(parser, data, length, finished ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
      const XML_Error code = parser ? XML_GetErrorCode(parser) : XML_ERROR_UNCLOSED_TOKEN;
      const XML_LChar* description = XML_ErrorString(code);
      LOG_ERR("NUT",
              "WebDAV XML parse failed: code=%d (%s), line=%lu, col=%lu, final=%d, chunk=%d, heap=%u, max alloc=%u",
              static_cast<int>(code), description ? description : "unknown",
              static_cast<unsigned long>(parser ? XML_GetCurrentLineNumber(parser) : 0),
              static_cast<unsigned long>(parser ? XML_GetCurrentColumnNumber(parser) : 0), finished ? 1 : 0, length,
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      error = "WebDAV XML parse failed";
      return false;
    }
    return true;
  }
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
    if (st->error.empty() && !st->current.href.empty() && st->rootPath && st->stageFile) {
      st->current.relativePath = relativePathFromHref(st->current.href, *st->rootPath);
      if (!st->current.relativePath.empty() && !writeStageEntry(*st->stageFile, st->current)) {
        st->error = "Could not stage Nutstore directory entries";
      }
    }
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

bool NutstoreWebDavClient::listRecursive(const std::string& remotePath, EntryCallback onEntry, std::string& error) {
  Storage.remove(DAV_STAGE_PATH);
  const std::string rootUrl = buildCollectionUrl(remotePath);
  rootPath = urlPath(rootUrl);

  if (ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_TLS) {
    error = "Low TLS memory. Restart the device and try again.";
    return false;
  }

  DavStream stream;
  freeink::SecureHttpClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);
  client.setCACert(NUTSTORE_ROOT_CA);
  client.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  client.setBasicAuth(username, password);
  client.setReuse(true);

  static constexpr const char* BODY =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>";

  std::vector<std::string> queue = {rootUrl};
  queue.reserve(16);
  std::vector<std::string> seenDirs;
  seenDirs.reserve(16);
  size_t scannedDirs = 0;
  while (!queue.empty()) {
    const std::string url = queue.back();
    queue.pop_back();
    if (std::find(seenDirs.begin(), seenDirs.end(), url) != seenDirs.end()) continue;
    seenDirs.push_back(url);
    scannedDirs++;
    if (scannedDirs > MAX_ENUM_DIRS) {
      error = "Too many folders. Set smaller Remote Path.";
      client.end();
      Storage.remove(DAV_STAGE_PATH);
      return false;
    }

    bool responseReady = false;
    for (int attempt = 0; attempt <= MAX_INCOMPLETE_RETRIES; ++attempt) {
      HalFile stage;
      if (!Storage.openFileForWrite("NUT", DAV_STAGE_PATH, stage)) {
        error = "Could not create Nutstore directory staging file";
        client.end();
        Storage.remove(DAV_STAGE_PATH);
        return false;
      }

      if (!stream.begin(stage, rootPath) || !client.begin(url)) {
        error = stream.error.empty() ? "PROPFIND client setup failed" : stream.error;
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      client.addHeader("Depth", "1");
      client.addHeader("Content-Type", "application/xml; charset=utf-8");
      LOG_DBG("NUT", "PROPFIND %s (attempt %d, heap: %u, max alloc: %u)", url.c_str(), attempt + 1,
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      const int status = client.sendRequest(
          "PROPFIND", reinterpret_cast<const uint8_t*>(BODY), strlen(BODY),
          [&stream](const uint8_t* data, size_t length) {
            if (stream.error.empty() && stream.state.error.empty()) {
              stream.feed(reinterpret_cast<const char*>(data), static_cast<int>(length));
            }
            return true;
          });
      const bool responseComplete = client.responseComplete();

      if (status != 207 && status != 200) {
        LOG_ERR("NUT", "PROPFIND returned HTTP %d (TLS/HTTP transport, XML=%s, heap=%u, max alloc=%u)", status,
                stream.error.empty() ? "ok" : stream.error.c_str(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        error = "PROPFIND failed: HTTP " + std::to_string(status);
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }

      if (!stream.state.error.empty()) {
        error = stream.state.error;
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }

      if (!responseComplete && attempt < MAX_INCOMPLETE_RETRIES) {
        LOG_INF("NUT", "Incomplete PROPFIND response for %s; retrying on a fresh connection", url.c_str());
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        delay(INCOMPLETE_RETRY_DELAY_MS);
        continue;
      }

      if (!responseComplete) {
        LOG_ERR("NUT", "Incomplete PROPFIND response after retry: HTTP %d, heap=%u, max alloc=%u", status,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        error = "PROPFIND response remained incomplete after retry";
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      if (!stream.error.empty()) {
        LOG_ERR("NUT", "PROPFIND XML failed with HTTP %d: %s (heap=%u, max alloc=%u)", status,
                stream.error.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        error = stream.error;
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      if (!stream.feed(nullptr, 0, true)) {
        LOG_ERR("NUT", "PROPFIND XML finalization failed with HTTP %d: %s (heap=%u, max alloc=%u)", status,
                stream.error.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        error = stream.error;
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      if (!stream.state.error.empty()) {
        error = stream.state.error;
        stage.close();
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      if (!stage.close()) {
        error = "Could not flush Nutstore directory staging file";
        Storage.remove(DAV_STAGE_PATH);
        client.end();
        return false;
      }
      responseReady = true;
      break;
    }

    if (!responseReady) {
      error = "PROPFIND response remained incomplete after retry";
      client.end();
      Storage.remove(DAV_STAGE_PATH);
      return false;
    }

    HalFile stagedEntries;
    if (!Storage.openFileForRead("NUT", DAV_STAGE_PATH, stagedEntries)) {
      error = "Could not read Nutstore directory staging file";
      client.end();
      Storage.remove(DAV_STAGE_PATH);
      return false;
    }
    while (stagedEntries.available() > 0) {
      NutstoreRemoteEntry entry;
      if (!readStageEntry(stagedEntries, entry)) {
        error = "Nutstore directory staging file is invalid";
        stagedEntries.close();
        client.end();
        Storage.remove(DAV_STAGE_PATH);
        return false;
      }
      if (entry.isDirectory) {
        std::string child = entry.href.rfind("http", 0) == 0 ? entry.href : origin + entry.href;
        queue.push_back(ensureTrailingSlash(child));
      } else if (!onEntry || !onEntry(std::move(entry), error)) {
        if (error.empty()) error = "Could not store Nutstore file list";
        stagedEntries.close();
        client.end();
        Storage.remove(DAV_STAGE_PATH);
        return false;
      }
    }
    stagedEntries.close();
    Storage.remove(DAV_STAGE_PATH);
  }
  client.end();
  Storage.remove(DAV_STAGE_PATH);
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

  LOG_DBG("NUT", "GET %s (heap: %u, max alloc: %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  freeink::SecureHttpClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);
  client.setCACert(NUTSTORE_ROOT_CA);
  client.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  client.setBasicAuth(username, password);
  client.setReuse(false);
  if (!client.begin(url)) {
    file.close();
    Storage.remove(tmpPath.c_str());
    error = "Invalid Nutstore download URL";
    return false;
  }

  size_t downloaded = 0;
  bool writeFailed = false;
  const int status = client.GET([&](const uint8_t* data, size_t length) {
    if (client.getStatus() != 200) return true;
    if (file.write(data, length) != length) {
      writeFailed = true;
      return false;
    }
    downloaded += length;
    const size_t total = client.hasContentLength() ? client.getContentLength() : entry.size;
    if (progress && total > 0) progress(downloaded, total);
    return true;
  });
  const bool responseComplete = client.responseComplete();
  const bool callbackAborted = client.callbackAborted();
  client.end();
  if (status != 200) {
    file.close();
    Storage.remove(tmpPath.c_str());
    error = "Download failed: HTTP " + std::to_string(status);
    return false;
  }
  if (writeFailed) {
    file.close();
    Storage.remove(tmpPath.c_str());
    error = "SD write failed";
    return false;
  }
  if (callbackAborted || !responseComplete) {
    file.close();
    Storage.remove(tmpPath.c_str());
    error = "Download response was incomplete";
    return false;
  }
  file.close();

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
