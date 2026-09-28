#include "NutstoreConfigStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <utility>

namespace {
constexpr const char* CONFIG_PATH = "/.crosspoint/nutstore.json";

std::string lowercase(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

std::string normalizeRemotePath(const std::string& path) {
  if (path.empty()) return "/";
  std::string out = path[0] == '/' ? path : "/" + path;
  return out;
}
}  // namespace

NutstoreConfigStore NutstoreConfigStore::instance;

bool NutstoreConfigStore::normalizeLocalPath(const std::string& path, std::string& normalized) {
  normalized.clear();
  if (path.empty() || path.size() > MAX_LOCAL_PATH_BYTES || path.front() != '/') return false;

  normalized.reserve(path.size());
  std::string firstComponent;
  size_t start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    if (i > start) {
      const size_t length = i - start;
      if (path.compare(start, length, ".") == 0 || path.compare(start, length, "..") == 0 ||
          path[i - 1] == '.' || path[i - 1] == ' ') {
        return false;
      }
      for (size_t j = start; j < i; ++j) {
        const unsigned char ch = static_cast<unsigned char>(path[j]);
        if (ch < 0x20 || ch == 0x7f || ch == '\\' || ch == ':' || ch == '<' || ch == '>' || ch == '"' ||
            ch == '|' || ch == '?' || ch == '*') {
          return false;
        }
      }
      if (firstComponent.empty()) firstComponent = lowercase(path.substr(start, length));
      normalized.push_back('/');
      normalized.append(path, start, length);
    }
    start = i + 1;
  }

  if (normalized.empty()) return false;  // Never allow mirroring the whole SD card.

  if (firstComponent == ".crosspoint" || firstComponent == "xtcache" ||
      firstComponent == "system volume information") {
    return false;
  }
  return true;
}

bool NutstoreConfigStore::loadFromFile() {
  if (!Storage.exists(CONFIG_PATH)) {
    return true;
  }

  const String json = Storage.readFile(CONFIG_PATH);
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("NUT", "config parse failed: %s", err.c_str());
    return false;
  }

  config.enabled = doc["enabled"] | false;
  config.baseUrl = doc["baseUrl"] | std::string("https://dav.jianguoyun.com/dav/");
  config.username = doc["username"] | std::string("");
  bool ok = false;
  config.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || config.password.empty()) {
    config.password = doc["password"] | std::string("");
  }
  config.remotePath = normalizeRemotePath(doc["remotePath"] | std::string("/"));
  const std::string savedLocalPath = doc["localPath"] | std::string("/Nutstore");
  if (!normalizeLocalPath(savedLocalPath, config.localPath)) {
    LOG_INF("NUT", "Invalid local path in config; using /Nutstore");
    config.localPath = "/Nutstore";
  }
  config.recursive = doc["recursive"] | true;
  config.mirrorDelete = doc["mirrorDelete"] | true;
  return true;
}

bool NutstoreConfigStore::saveToFile() const {
  std::string localPath;
  if (!normalizeLocalPath(config.localPath, localPath)) {
    LOG_ERR("NUT", "Refusing to save invalid local path");
    return false;
  }

  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["enabled"] = config.enabled;
  doc["baseUrl"] = config.baseUrl;
  doc["username"] = config.username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(config.password);
  doc["remotePath"] = normalizeRemotePath(config.remotePath);
  doc["localPath"] = localPath;
  doc["recursive"] = true;
  doc["mirrorDelete"] = true;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(CONFIG_PATH, json);
}
