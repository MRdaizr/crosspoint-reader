#include "NutstoreConfigStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <utility>

namespace {
constexpr const char* CONFIG_PATH = "/.crosspoint/nutstore.json";

std::string normalizeLocalPath(const std::string& path) {
  if (path.empty() || path == "/") return "/Nutstore";
  std::string out = path[0] == '/' ? path : "/" + path;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out == "/Nutstore" ? out : "/Nutstore";
}

std::string normalizeRemotePath(const std::string& path) {
  if (path.empty()) return "/";
  std::string out = path[0] == '/' ? path : "/" + path;
  return out;
}
}  // namespace

NutstoreConfigStore NutstoreConfigStore::instance;

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
  config.localPath = normalizeLocalPath(doc["localPath"] | std::string("/Nutstore"));
  config.recursive = doc["recursive"] | true;
  config.mirrorDelete = doc["mirrorDelete"] | true;
  return true;
}

bool NutstoreConfigStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["enabled"] = config.enabled;
  doc["baseUrl"] = config.baseUrl;
  doc["username"] = config.username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(config.password);
  doc["remotePath"] = normalizeRemotePath(config.remotePath);
  doc["localPath"] = "/Nutstore";
  doc["recursive"] = true;
  doc["mirrorDelete"] = true;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(CONFIG_PATH, json);
}
