#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FirmwareFlasher.h"
#include "HttpDownloader.h"
#include "LibraryWebApi.h"
#include "NutstoreConfigStore.h"
#include "OpdsServerStore.h"
#include "ProtectedPaths.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "TodoStore.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FirmwarePageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/TodoPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "html/shared/PluginHost.js.inc"
#include "network/NutstoreSync.h"
#include "util/BookCacheUtils.h"
#include "util/BookmarkUtil.h"
#include "util/PluginCrypto.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/PluginPermissions.h"
#include "util/TaskWatchdog.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr const char* WEB_FIRMWARE_PATH = "/.crosspoint/web_firmware.bin";
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t MAX_TRACKED_BOOKS_PER_PATH_MOVE = 256;
constexpr size_t MAX_DIRECTORY_MOVE_ENTRIES = 8192;
constexpr uint8_t MAX_DIRECTORY_MOVE_DEPTH = 16;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
HalFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (name.equalsIgnoreCase(item)) {
      return true;
    }
  }
  return false;
}

bool isProtectedPath(const String& path) {
  if (protectedpaths::isSensitivePath(path.c_str())) return true;
  size_t start = 1;
  for (size_t i = 1; i <= path.length(); ++i) {
    if (i != path.length() && path.charAt(i) != '/') continue;
    if (i > start && isProtectedItemName(path.substring(start, i))) return true;
    start = i + 1;
  }
  return false;
}

bool isPathAtOrBelow(const String& path, const String& prefix) {
  if (path.length() < prefix.length() || !path.substring(0, prefix.length()).equalsIgnoreCase(prefix)) return false;
  return path.length() == prefix.length() || path.charAt(prefix.length()) == '/';
}

bool containsOpenReaderBook(const String& path) {
  if (!activityManager.isReaderActivity()) return false;
  const String openPath = APP_STATE.openEpubPath.c_str();
  return !openPath.isEmpty() && isPathAtOrBelow(openPath, path);
}

struct BookPathMove {
  std::string oldPath;
  std::string newPath;
  bool moveCache = false;
  bool moveBookmarks = false;
  bool cacheMoved = false;
  bool bookmarksMoved = false;
};

struct PathMovePlan {
  std::vector<BookPathMove> books;
  size_t visitedEntries = 0;
};

bool addBookPathMove(const std::string& oldPath, const std::string& newPath, PathMovePlan& plan, std::string& error) {
  const std::string oldCachePath = getBookCachePath(oldPath);
  if (oldCachePath.empty()) return true;
  const std::string newCachePath = getBookCachePath(newPath);
  const bool isEpub = FsHelpers::hasEpubExtension(oldPath);
  const std::string oldBookmarkPath = isEpub ? BookmarkUtil::getBookmarkPath(oldPath) : std::string{};
  const std::string newBookmarkPath = isEpub ? BookmarkUtil::getBookmarkPath(newPath) : std::string{};

  const bool oldCacheExists = Storage.exists(oldCachePath.c_str());
  if (oldCachePath != newCachePath && Storage.exists(newCachePath.c_str())) {
    error = "A reader cache already exists for the destination book path";
    return false;
  }
  if (oldCacheExists && oldCachePath != newCachePath) {
    for (const auto& planned : plan.books) {
      if (planned.moveCache && getBookCachePath(planned.newPath) == newCachePath) {
        error = "Multiple books map to the same destination cache";
        return false;
      }
      if (planned.moveCache && getBookCachePath(planned.oldPath) == oldCachePath) {
        error = "Multiple books share a source cache; refusing an unsafe move";
        return false;
      }
    }
  }

  const bool oldBookmarkExists = isEpub && Storage.exists(oldBookmarkPath.c_str());
  if (isEpub && oldBookmarkPath != newBookmarkPath && Storage.exists(newBookmarkPath.c_str())) {
    error = "A bookmark file already exists for the destination book path";
    return false;
  }
  if (oldBookmarkExists && oldBookmarkPath != newBookmarkPath) {
    for (const auto& planned : plan.books) {
      if (!planned.moveBookmarks) continue;
      if (BookmarkUtil::getBookmarkPath(planned.newPath) == newBookmarkPath) {
        error = "Multiple books map to the same destination bookmark file";
        return false;
      }
      if (BookmarkUtil::getBookmarkPath(planned.oldPath) == oldBookmarkPath) {
        error = "Multiple books share a source bookmark file; refusing an unsafe move";
        return false;
      }
    }
  }

  const bool moveCache = oldCacheExists && oldCachePath != newCachePath;
  const bool moveBookmarks = oldBookmarkExists && oldBookmarkPath != newBookmarkPath;
  if (!moveCache && !moveBookmarks) return true;
  if (plan.books.size() >= MAX_TRACKED_BOOKS_PER_PATH_MOVE) {
    error = "Too many cached/bookmarked books to relocate safely in one operation";
    return false;
  }
  plan.books.push_back({oldPath, newPath, moveCache, moveBookmarks, false, false});
  return true;
}

bool collectDirectoryBookMoves(const std::string& oldDir, const std::string& newDir, const uint8_t depth,
                               PathMovePlan& plan, std::string& error) {
  if (depth > MAX_DIRECTORY_MOVE_DEPTH) {
    error = "Folder nesting is too deep to scan safely";
    return false;
  }
  HalFile directory = Storage.open(oldDir.c_str());
  if (!directory || !directory.isDirectory()) {
    error = "Could not scan the selected folder";
    return false;
  }

  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (++plan.visitedEntries > MAX_DIRECTORY_MOVE_ENTRIES) {
      entry.close();
      directory.close();
      error = "Folder contains too many entries to relocate safely";
      return false;
    }

    char name[256] = {};
    const size_t nameLength = entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (nameLength == 0 || nameLength >= sizeof(name) ||
        !FsHelpers::isSafePathComponent(std::string_view(name, nameLength))) {
      directory.close();
      error = "Folder contains an invalid or overlong item name";
      return false;
    }

    const std::string oldPath = oldDir == "/" ? "/" + std::string(name) : oldDir + "/" + name;
    const std::string suffix = oldPath.substr(oldDir.size());
    const std::string newPath = newDir == "/" ? suffix : newDir + suffix;
    bool ok = true;
    if (isDirectory) {
      ok = collectDirectoryBookMoves(oldPath, newPath, static_cast<uint8_t>(depth + 1), plan, error);
    } else {
      ok = addBookPathMove(oldPath, newPath, plan, error);
    }
    if (!ok) {
      directory.close();
      return false;
    }
    yield();
    resetTaskWatchdogIfSubscribed();
  }
  directory.close();
  return true;
}

bool preparePathMove(const String& oldPath, const String& newPath, const bool isDirectory, PathMovePlan& plan,
                     std::string& error) {
  plan = {};
  if (isDirectory) {
    return collectDirectoryBookMoves(oldPath.c_str(), newPath.c_str(), 0, plan, error);
  }
  return addBookPathMove(oldPath.c_str(), newPath.c_str(), plan, error);
}

void rollbackPathArtifacts(PathMovePlan& plan) {
  for (auto it = plan.books.rbegin(); it != plan.books.rend(); ++it) {
    if (it->bookmarksMoved) {
      const std::string from = BookmarkUtil::getBookmarkPath(it->newPath);
      const std::string to = BookmarkUtil::getBookmarkPath(it->oldPath);
      if (!Storage.rename(from.c_str(), to.c_str())) {
        LOG_ERR("WEB", "Could not roll back bookmark migration: %s -> %s", from.c_str(), to.c_str());
      }
      it->bookmarksMoved = false;
    }
    if (it->cacheMoved) {
      const std::string from = getBookCachePath(it->newPath);
      const std::string to = getBookCachePath(it->oldPath);
      if (!Storage.rename(from.c_str(), to.c_str())) {
        LOG_ERR("WEB", "Could not roll back cache migration: %s -> %s", from.c_str(), to.c_str());
      }
      it->cacheMoved = false;
    }
  }
}

bool applyPathArtifacts(PathMovePlan& plan, std::string& error) {
  for (auto& book : plan.books) {
    if (book.moveCache) {
      const std::string from = getBookCachePath(book.oldPath);
      const std::string to = getBookCachePath(book.newPath);
      if (!Storage.rename(from.c_str(), to.c_str())) {
        error = "Could not migrate a book cache; no files were moved";
        rollbackPathArtifacts(plan);
        return false;
      }
      book.cacheMoved = true;
    }
    if (book.moveBookmarks) {
      const std::string from = BookmarkUtil::getBookmarkPath(book.oldPath);
      const std::string to = BookmarkUtil::getBookmarkPath(book.newPath);
      if (!Storage.rename(from.c_str(), to.c_str())) {
        error = "Could not migrate a bookmark file; no files were moved";
        rollbackPathArtifacts(plan);
        return false;
      }
      book.bookmarksMoved = true;
    }
  }
  return true;
}

bool updatePathReferences(const String& oldPath, const String& newPath) {
  const std::string oldPrefix = oldPath.c_str();
  const std::string newPrefix = newPath.c_str();
  bool saved = true;
  if (!RECENT_BOOKS.updatePathPrefix(oldPrefix, newPrefix)) {
    LOG_ERR("WEB", "Failed to persist recent-book paths after relocating %s", oldPrefix.c_str());
    saved = false;
  }
  if (!READING_STATS.updateBookPathPrefix(oldPrefix, newPrefix)) {
    LOG_ERR("WEB", "Failed to persist reading-stat paths after relocating %s", oldPrefix.c_str());
    saved = false;
  }
  const String openPath = APP_STATE.openEpubPath.c_str();
  if (!openPath.isEmpty() && isPathAtOrBelow(openPath, oldPath)) {
    APP_STATE.openEpubPath = newPrefix + std::string(openPath.c_str()).substr(oldPrefix.size());
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("WEB", "Failed to persist resume path after relocating %s", oldPrefix.c_str());
      saved = false;
    }
  }
  return saved;
}

const char* firmwareFlashResultMessage(firmware_flash::Result result) {
  switch (result) {
    case firmware_flash::Result::OK:
      return "Firmware installed. Device is restarting.";
    case firmware_flash::Result::OPEN_FAIL:
      return "Could not open uploaded firmware.";
    case firmware_flash::Result::TOO_SMALL:
      return "Firmware file is too small.";
    case firmware_flash::Result::TOO_LARGE:
      return "Firmware file is too large for the OTA partition.";
    case firmware_flash::Result::BAD_MAGIC:
      return "Firmware is not a valid ESP32 image.";
    case firmware_flash::Result::BAD_SEGMENTS:
      return "Firmware segment table is invalid.";
    case firmware_flash::Result::BAD_CHECKSUM:
      return "Firmware checksum failed.";
    case firmware_flash::Result::BAD_SHA:
      return "Firmware SHA256 verification failed.";
    case firmware_flash::Result::BAD_CHIP:
      return "Firmware is for a different device.";
    case firmware_flash::Result::WRONG_BOARD:
      return "Firmware is for a different board.";
    case firmware_flash::Result::BAD_SIZE:
      return "Firmware image size is invalid.";
    case firmware_flash::Result::NO_PARTITION:
      return "No OTA partition is available.";
    case firmware_flash::Result::OOM:
      return "Not enough memory to process firmware.";
    case firmware_flash::Result::READ_FAIL:
      return "Could not read uploaded firmware.";
    case firmware_flash::Result::ERASE_FAIL:
      return "Could not erase OTA partition.";
    case firmware_flash::Result::WRITE_FAIL:
      return "Could not write OTA partition.";
    case firmware_flash::Result::OTADATA_FAIL:
      return "Firmware was written, but boot switch failed.";
  }
  return "Firmware update failed.";
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
namespace {
bool pluginSafeFile(const std::string& name) {
  if (name.empty() || name.size() > 128 || name.front() == '/' || name.find("..") != std::string::npos) return false;
  return protectedpaths::isPluginPath("/probe/" + name);
}
const char* pluginMime(const std::string& name) {
  const size_t dot = name.rfind('.');
  const std::string extension = dot == std::string::npos ? "" : name.substr(dot);
  if (extension == ".js") return "application/javascript";
  if (extension == ".css") return "text/css";
  if (extension == ".html") return "text/html";
  if (extension == ".json") return "application/json";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".md") return "text/plain";
  return "application/octet-stream";
}
}  // namespace

bool CrossPointWebServer::pluginSessionAllowed(bool respond) const {
  if (!server || !pluginSession[0]) return false;
  const String supplied =
      server->hasHeader("X-Plugin-Session") ? server->header("X-Plugin-Session") : server->arg("session");
  if (supplied != pluginSession) {
    if (respond) server->send(403, "application/json", "{\"error\":\"invalid web session\"}");
    return false;
  }
  // Browser mutations must come from this origin. Non-browser API callers use
  // the capability acquired from /api/plugins during this web-server session.
  const String origin = server->header("Origin");
  if (!origin.isEmpty() && origin != String("http://") + server->hostHeader()) {
    if (respond) server->send(403, "application/json", "{\"error\":\"cross-origin request\"}");
    return false;
  }
  return true;
}
bool CrossPointWebServer::pluginAuthorized(const char* plugin) const {
  if (!pluginSessionAllowed()) return false;
  if (!PluginLocations::validName(plugin) || !PluginPermissions::allowed(plugin)) {
    server->send(403, "application/json", "{\"error\":\"plugin disabled or changed; approval required\"}");
    return false;
  }
  return true;
}
bool CrossPointWebServer::readPluginJson(JsonDocument& doc) const {
  if (!server->hasArg("plain") || server->arg("plain").length() > 16 * 1024) {
    server->send(413, "application/json", "{\"error\":\"missing or oversized body\"}");
    return false;
  }
  if (deserializeJson(doc, server->arg("plain"), DeserializationOption::NestingLimit(16)) != DeserializationError::Ok) {
    server->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return false;
  }
  return true;
}
void CrossPointWebServer::sendPluginJson(const JsonDocument& doc) const {
  String body;
  if (measureJson(doc) > 24 * 1024 || !body.reserve(measureJson(doc))) {
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  serializeJson(doc, body);
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", body);
}
void CrossPointWebServer::handlePluginList() const {
  const String origin = server->header("Origin");
  if (!origin.isEmpty() && origin != String("http://") + server->hostHeader()) {
    server->send(403, "application/json", "{\"error\":\"cross-origin request\"}");
    return;
  }
  JsonDocument result;
  auto list = result.to<JsonArray>();
  for (const auto& entry : PluginLocations::scanPlugins()) {
    auto record = list.add<JsonObject>();
    const auto status = PluginPermissions::inspect(entry.name.c_str());
    record["name"] = entry.name;
    record["dir"] = entry.dir;
    record["title"] = entry.name;
    record["description"] = "";
    record["script"] = entry.hasPluginJs ? "plugin.js" : (entry.hasMainJs ? "main.js" : "");
    record["mount"] = "settings";
    record["enabled"] = status.enabled;
    record["approved"] = status.approved;
    record["available"] = status.available;
    record["systemEnabled"] = PluginPermissions::systemEnabled();
    record["digest"] = status.digest;
    std::string raw;
    if (entry.hasManifest && pluginhttp::readFile(entry.dir + "/manifest.json", 8192, raw)) {
      JsonDocument metadata, filter;
      filter["title"] = true;
      filter["description"] = true;
      filter["mount"] = true;
      filter["version"] = true;
      if (deserializeJson(metadata, raw, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
        const char* title = metadata["title"] | "";
        const char* description = metadata["description"] | "";
        const char* mount = metadata["mount"] | "settings";
        if (strlen(title) <= 128 && *title) record["title"] = title;
        if (strlen(description) <= 512) record["description"] = description;
        if (strcmp(mount, "files") == 0 || strcmp(mount, "settings") == 0) record["mount"] = mount;
        record["version"] = metadata["version"] | "";
      }
    }
  }
  server->sendHeader("X-Plugin-Session", pluginSession);
  sendPluginJson(result);
}
void CrossPointWebServer::handlePluginPermission() {
  if (!pluginSessionAllowed()) return;
  JsonDocument req;
  if (!readPluginJson(req)) return;
  const char* name = req["plugin"] | "";
  if (!PluginPermissions::setEnabled(name, req["enabled"] | false, req["digest"] | "")) {
    server->send(409, "application/json", "{\"error\":\"plugin changed or approval could not be saved\"}");
    return;
  }
  pluginevents::refreshSubscriptions();
  server->send(200, "application/json", "{\"ok\":true}");
}
void CrossPointWebServer::handlePluginFile() const {
  const String name = server->arg("name");
  if (!pluginAuthorized(name.c_str())) return;
  const std::string file = server->arg("file").c_str();
  const auto dir = PluginLocations::findPluginDir(name.c_str());
  const auto path = dir + "/" + file;
  if (!pluginSafeFile(file) || !protectedpaths::isPluginPath(path)) {
    server->send(400, "text/plain", "invalid plugin path");
    return;
  }
  HalFile input = Storage.open(path.c_str());
  if (!input || input.isDirectory() || input.fileSize64() > 256 * 1024) {
    server->send(404, "text/plain", "not found or too large");
    return;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(256);
  if (!buffer) {
    server->send(503, "text/plain", "out of memory");
    return;
  }
  server->sendHeader("Cache-Control", "no-store");
  server->sendHeader("X-Content-Type-Options", "nosniff");
  server->setContentLength(input.fileSize());
  server->send(200, pluginMime(file), "");
  auto client = server->client();
  while (input.available() && client.connected()) {
    const int n = input.read(buffer.get(), 256);
    if (n <= 0 || client.write(buffer.get(), n) != static_cast<size_t>(n)) {
      client.stop();
      break;
    }
    resetTaskWatchdogIfSubscribed();
  }
}
void CrossPointWebServer::handlePluginHost() const {
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "application/javascript", PLUGIN_HOST_JS, sizeof(PLUGIN_HOST_JS) - 1);
}
void CrossPointWebServer::handlePluginRunnerPage() const {
  static constexpr char RUNNER[] PROGMEM =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Plugin runner</title></head>"
      "<body><h1>Plugin runner</h1><p>Keep this page open to run approved browser actions.</p>"
      "<div id=\"plugin-container\"></div><script src=\"/js/plugin-host.js\"></script>"
      "<script>loadPlugins(null)</script></body></html>";
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "text/html", RUNNER, sizeof(RUNNER) - 1);
}
void CrossPointWebServer::handlePluginJobSubmit() {
  JsonDocument req;
  if (!readPluginJson(req)) return;
  const char* plugin = req["plugin"] | "";
  if (!pluginAuthorized(plugin)) return;
  const char* action = req["action"] | "";
  if (!PluginJobPool::validAction(action) || (!req["args"].isNull() && measureJson(req["args"]) >= 192)) {
    server->send(400, "application/json", "{\"error\":\"invalid action or args >=192 bytes\"}");
    return;
  }
  auto args = makeUniqueNoThrow<char[]>(192);
  if (!args) {
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  if (req["args"].isNull())
    strcpy(args.get(), "{}");
  else
    serializeJson(req["args"], args.get(), 192);
  auto* job = pluginJobs.submit(plugin, action, args.get(), millis(), true);
  if (!job) {
    server->send(503, "application/json", "{\"error\":\"job pool full\"}");
    return;
  }
  JsonDocument response;
  response["id"] = job->id;
  sendPluginJson(response);
}
void CrossPointWebServer::handlePluginJobClaim() {
  const String plugin = server->arg("plugin");
  if (!pluginAuthorized(plugin.c_str())) return;
  auto* job = pluginJobs.claim(plugin.c_str(), millis(), true);
  JsonDocument response;
  response["id"] = job ? job->id : 0;
  if (job) {
    response["claim"] = job->claim;
    response["action"] = job->action;
    JsonDocument args;
    deserializeJson(args, job->args);
    response["args"] = args.as<JsonVariantConst>();
  }
  sendPluginJson(response);
}
void CrossPointWebServer::handlePluginJobComplete() {
  JsonDocument req;
  if (!readPluginJson(req)) return;
  auto* job = pluginJobs.find(req["id"] | 0u);
  // Authorize even unknown ids: no guessed id exposes another plugin's result.
  const char* plugin = req["plugin"] | (job ? job->plugin : "");
  if (!pluginAuthorized(plugin)) return;
  if (!job || strcmp(plugin, job->plugin) != 0) {
    server->send(404, "application/json", "{\"error\":\"unknown job\"}");
    return;
  }
  if (!req["result"].isNull() && measureJson(req["result"]) >= 192) {
    server->send(413, "application/json", "{\"error\":\"result >=192 bytes\"}");
    return;
  }
  auto result = makeUniqueNoThrow<char[]>(192);
  if (!result) {
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  if (req["result"].isNull())
    strcpy(result.get(), "null");
  else
    serializeJson(req["result"], result.get(), 192);
  if (!pluginJobs.complete(job->id, req["claim"] | 0u, req["ok"] | false, result.get(), millis(), true)) {
    server->send(409, "application/json", "{\"error\":\"stale lease\"}");
    return;
  }
  server->send(200, "application/json", "{\"ok\":true}");
}
void CrossPointWebServer::handlePluginJobStatus() {
  auto* job = pluginJobs.find(strtoul(server->arg("id").c_str(), nullptr, 10));
  const String plugin = server->arg("plugin");
  if (!pluginAuthorized(plugin.c_str())) return;
  JsonDocument response;
  if (!job || plugin != job->plugin) {
    response["state"] = "unknown";
    response["result"] = nullptr;
  } else {
    static constexpr const char* STATES[] = {"empty", "pending", "running", "done", "error"};
    response["id"] = job->id;
    response["state"] = STATES[job->state];
    JsonDocument result;
    deserializeJson(result, job->result[0] ? job->result : "null");
    response["result"] = result.as<JsonVariantConst>();
  }
  sendPluginJson(response);
}
void CrossPointWebServer::suspendPluginTransferServices() {
  // Active WS uploads reject relays/fetches in their handlers; closing a live
  // upload's socket here would strand its caller and destroy local lifecycle.
  if (wsServer) {
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
  }
  if (udpActive) {
    udp.stop();
    udpActive = false;
  }
}
void CrossPointWebServer::resumePluginTransferServices() {
  if (!running) return;
  wsServer = makeUniqueNoThrow<WebSocketsServer>(wsPort);
  if (wsServer) {
    wsInstance = this;
    wsServer->begin();
    wsServer->onEvent(wsEventCallback);
  } else
    LOG_ERR("WEB", "OOM: resume WebSocket");
  udpActive = udp.begin(LOCAL_UDP_PORT);
}
void CrossPointWebServer::handleRelay() {
  JsonDocument req;
  if (!readPluginJson(req)) return;
  const std::string plugin = req["plugin"] | "";
  if (!pluginAuthorized(plugin.c_str())) return;
  if (wsUploadInProgress) {
    server->send(409, "application/json", "{\"error\":\"upload in progress\"}");
    return;
  }
  const std::string url = req["url"] | "", method = req["method"] | "GET", body = req["body"] | "";
  pluginhttp::Headers headers;
  pluginhttp::readHeaders(req["headers"], headers);
  req.clear();
  req.shrinkToFit();
  suspendPluginTransferServices();
  const auto abort = [&] {
    resetTaskWatchdogIfSubscribed();
    return !PluginPermissions::systemEnabled() || (uploadCancelCheck && uploadCancelCheck());
  };
  String content;
  pluginhttp::Headers responseHeaders;
  const int status = pluginhttp::request(nullptr, url, method, body, headers, content, 32 * 1024, &responseHeaders,
                                         abort, millis() + 30000);
  resumePluginTransferServices();
  if (status < 0) {
    server->send(502, "application/json", "{\"error\":\"relay failed or response too large\"}");
    return;
  }
  JsonDocument doc;
  auto list = doc.to<JsonArray>();
  for (const auto& header : responseHeaders) {
    auto pair = list.add<JsonArray>();
    pair.add(header.first);
    pair.add(header.second);
  }
  String rawHeaders;
  serializeJson(doc, rawHeaders);
  server->sendHeader("X-Relay-Status", String(status));
  server->sendHeader("X-Relay-Headers", rawHeaders);
  server->send(200, "application/octet-stream", content);
}
void CrossPointWebServer::handleFetch() {
  JsonDocument req;
  if (!readPluginJson(req)) return;
  const std::string plugin = req["plugin"] | "", url = req["url"] | "", dest = req["dest"] | "";
  if (!pluginAuthorized(plugin.c_str())) return;
  if (wsUploadInProgress) {
    server->send(409, "application/json", "{\"error\":\"upload in progress\"}");
    return;
  }
  if (!protectedpaths::isPluginPath(dest) || !protectedpaths::isPluginPath(dest + ".part") ||
      !protectedpaths::isPluginPath(dest + ".bak") || (req["offset"] | 0u) != 0) {
    server->send(400, "application/json", "{\"error\":\"invalid destination/offset\"}");
    return;
  }
  const size_t slash = dest.rfind('/');
  if (slash > 0 && !Storage.ensureDirectoryExists(dest.substr(0, slash).c_str())) {
    server->send(500, "application/json", "{\"error\":\"cannot create parent\"}");
    return;
  }
  pluginhttp::Headers headers;
  pluginhttp::readHeaders(req["headers"], headers);
  req.clear();
  req.shrinkToFit();
  bool cancel = false;
  suspendPluginTransferServices();
  const auto status = HttpDownloader::downloadToFile(
      url, dest,
      [&](size_t, size_t) {
        resetTaskWatchdogIfSubscribed();
        cancel = !PluginPermissions::systemEnabled() || (uploadCancelCheck && uploadCancelCheck());
      },
      &cancel, "", "", headers, 32 * 1024 * 1024, millis() + 120000,
      [&] { return !PluginPermissions::systemEnabled() || (uploadCancelCheck && uploadCancelCheck()); });
  resumePluginTransferServices();
  if (status != HttpDownloader::OK) {
    server->send(status == HttpDownloader::UNAUTHORIZED ? 401 : 502, "application/json",
                 "{\"error\":\"download failed\"}");
    return;
  }
  invalidateBookCache(dest);
  const pluginevents::Var vars[] = {{"path", dest.c_str()}, {"title", dest.c_str()}, {"plugin", plugin.c_str()}};
  pluginevents::emit(pluginevents::Event::BookDownloaded, vars, 3);
  HalFile file = Storage.open(dest.c_str());
  JsonDocument response;
  response["ok"] = true;
  response["complete"] = true;
  response["bytes"] = file ? file.fileSize() : 0;
  sendPluginJson(response);
}
void CrossPointWebServer::handlePluginFsUpload() {
  const auto& part = server->upload();
  auto& state = pluginUpload;
  const auto fail = [&](int error) {
    if (state.file.isOpen()) state.file.close();
    if (!state.tmp.empty()) Storage.remove(state.tmp.c_str());
    state.error = error;
  };
  if (part.status == UPLOAD_FILE_START) {
    if (state.started) {
      fail(400);
      return;
    }
    state = PluginUpload{};
    state.started = true;
    state.plugin = server->arg("plugin").c_str();
    state.path = server->arg("path").c_str();
    if (!pluginSessionAllowed(false) || !PluginPermissions::allowed(state.plugin.c_str())) {
      state.error = 403;
      return;
    }
    state.tmp = state.path + ".tmp";
    if (!protectedpaths::isPluginPath(state.path) || !protectedpaths::isPluginPath(state.tmp) ||
        !protectedpaths::isPluginPath(state.path + ".bak")) {
      state.tmp.clear();
      fail(400);
      return;
    }
    const size_t slash = state.path.rfind('/');
    if (slash > 0 && !Storage.ensureDirectoryExists(state.path.substr(0, slash).c_str())) {
      fail(500);
      return;
    }
    if (!Storage.openFileForWrite("PLG", state.tmp, state.file)) fail(500);
  } else if (part.status == UPLOAD_FILE_WRITE) {
    if (state.error || !state.started || !state.file) return;
    if (!PluginPermissions::systemEnabled() || (uploadCancelCheck && uploadCancelCheck())) {
      fail(403);
      return;
    }
    if (part.currentSize > 256 * 1024 - state.bytes) {
      fail(413);
      return;
    }
    if (state.file.write(part.buf, part.currentSize) != part.currentSize) {
      fail(500);
      return;
    }
    state.bytes += part.currentSize;
    resetTaskWatchdogIfSubscribed();
  } else if (part.status == UPLOAD_FILE_END) {
    if (!state.error && state.file) {
      state.file.flush();
      if (!state.file.close())
        fail(500);
      else
        state.ended = true;
    }
  } else if (part.status == UPLOAD_FILE_ABORTED)
    fail(400);
}
void CrossPointWebServer::handlePluginFs() {
  auto& state = pluginUpload;
  if (!state.error && (!state.started || !state.ended || !state.bytes)) state.error = 400;
  if (!state.error && (!pluginSessionAllowed(false) || !PluginPermissions::allowed(state.plugin.c_str())))
    state.error = 403;
  if (!state.error && !Storage.replaceFile(state.tmp.c_str(), state.path.c_str())) state.error = 500;
  if (state.error) {
    if (state.file.isOpen()) state.file.close();
    if (!state.tmp.empty()) Storage.remove(state.tmp.c_str());
    server->send(state.error, "application/json", "{\"error\":\"plugin upload rejected or incomplete\"}");
  } else {
    invalidateBookCache(state.path);
    JsonDocument response;
    response["ok"] = true;
    response["bytes"] = state.bytes;
    sendPluginJson(response);
  }
  state = PluginUpload{};
}
void CrossPointWebServer::handleCrypto() {
  JsonDocument req;
  if (!readPluginJson(req)) return;
  if (!pluginAuthorized(req["plugin"] | "")) return;
  JsonDocument response;
  int status = 200;
  if (!plugincrypto::process(req, response, status)) {
    String error;
    if (!error.reserve(measureJson(response)) || !serializeJson(response, error)) {
      server->send(503, "application/json", "{\"error\":\"out of memory\"}");
      return;
    }
    server->send(status, "application/json", error);
    return;
  }
  sendPluginJson(response);
}

CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server = makeUniqueNoThrow<WebServer>(port);

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/todos", HTTP_GET, [this] { handleTodosPage(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/api/library", HTTP_GET, [this] { handleLibraryGet(*server); });
  server->on("/api/library/rebuild", HTTP_POST, [this] {
    if (wsUploadInProgress) {
      server->send(409, "application/json", "{\"error\":\"upload_in_progress\"}");
      return;
    }
    const String origin = server->header("Origin");
    if (!origin.isEmpty() && origin != String("http://") + server->hostHeader()) {
      server->send(403, "application/json", "{\"error\":\"cross_origin_request\"}");
      return;
    }
    const library::BuildCallbacks callbacks{
        this,
        [](void* opaque) {
          auto* self = static_cast<CrossPointWebServer*>(opaque);
          resetTaskWatchdogIfSubscribed();
          return self->uploadCancelCheck && self->uploadCancelCheck();
        },
        [](void*, library::BuildPhase, uint16_t, uint16_t) { resetTaskWatchdogIfSubscribed(); }};
    suspendPluginTransferServices();
    handleLibraryRebuild(*server, callbacks);  // synchronous; never re-enter handleClient
    resumePluginTransferServices();
  });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  server->on("/api/nutstore/config", HTTP_GET, [this] { handleGetNutstoreConfig(); });
  server->on("/api/nutstore/config", HTTP_POST, [this] { handlePostNutstoreConfig(); });
  server->on("/api/nutstore/sync", HTTP_POST, [this] { handlePostNutstoreSync(); });
  server->on("/api/nutstore/status", HTTP_GET, [this] { handleGetNutstoreStatus(); });

  // To-do endpoints
  server->on("/api/todos", HTTP_GET, [this] { handleGetTodos(); });
  server->on("/api/todos", HTTP_POST, [this] { handlePostTodo(); });
  server->on("/api/todos/toggle", HTTP_POST, [this] { handleToggleTodo(); });
  server->on("/api/todos/delete", HTTP_POST, [this] { handleDeleteTodo(); });

  // Firmware update endpoints
  server->on("/firmware", HTTP_GET, [this] { handleFirmwarePage(); });
  server->on(
      "/api/firmware/upload", HTTP_POST, [this] { handleFirmwareUpload(); }, [this] { handleFirmwareUploadData(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  pluginJobs.reset(esp_random());
  for (size_t i = 0; i < 4; ++i) snprintf(pluginSession + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  server->on("/api/plugins", HTTP_GET, [this] { handlePluginList(); });
  server->on("/api/plugins/permission", HTTP_POST, [this] { handlePluginPermission(); });
  server->on("/plugin", HTTP_GET, [this] { handlePluginFile(); });
  server->on("/js/plugin-host.js", HTTP_GET, [this] { handlePluginHost(); });
  server->on("/plugins-run", HTTP_GET, [this] { handlePluginRunnerPage(); });
  server->on("/api/plugin-jobs", HTTP_POST, [this] { handlePluginJobSubmit(); });
  server->on("/api/plugin-jobs/claim", HTTP_GET, [this] { handlePluginJobClaim(); });
  server->on("/api/plugin-jobs/complete", HTTP_POST, [this] { handlePluginJobComplete(); });
  server->on("/api/plugin-jobs/status", HTTP_GET, [this] { handlePluginJobStatus(); });
  server->on("/api/relay", HTTP_POST, [this] { handleRelay(); });
  server->on("/api/fetch", HTTP_POST, [this] { handleFetch(); });
  server->on("/api/crypto", HTTP_POST, [this] { handleCrypto(); });
  server->on("/api/plugin-fs", HTTP_POST, [this] { handlePluginFs(); }, [this] { handlePluginFsUpload(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth",      "Destination", "Overwrite", "If",
                              "Lock-Token", "Timeout",     "Origin",    "X-Plugin-Session"};
  server->collectHeaders(davHeaders, 8);
  auto* dav = new (std::nothrow) WebDAVHandler();
  if (dav) server->addHandler(dav);  // WebServer owns and deletes this handler.
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer = makeUniqueNoThrow<WebSocketsServer>(wsPort);
  wsInstance = const_cast<CrossPointWebServer*>(this);
  if (wsServer) {
    wsServer->begin();
    wsServer->onEvent(wsEventCallback);
  }
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

bool CrossPointWebServer::dropUploadIfCancelled() const {
  if (!server || !uploadCancelCheck || !uploadCancelCheck()) return false;
  // Closing the client makes the next body read fail so WebServer reports
  // UPLOAD_FILE_ABORTED and the upload handler removes the partial file.
  server->client().stop();
  return true;
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  pluginJobs.reset();
  pluginSession[0] = '\0';
  if (pluginUpload.file.isOpen()) pluginUpload.file.close();
  if (!pluginUpload.tmp.empty()) Storage.remove(pluginUpload.tmp.c_str());
  pluginUpload = PluginUpload{};
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  const unsigned long requestStart = millis();
  server->handleClient();
  const unsigned long requestDuration = millis() - requestStart;
  if (requestDuration > 250) {
    LOG_DBG("WEB", "[HTTP] Request returned: method=%d uri=%s duration=%lu ms free=%d",
            static_cast<int>(server->method()), server->uri().c_str(), requestDuration, ESP.getFreeHeap());
  }

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  if (firmwareRestartPending && millis() >= firmwareRestartAt) {
    LOG_INF("WEB", "Restarting after web firmware update");
    ESP.restart();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

void CrossPointWebServer::setFirmwareStatus(FirmwareUpdatePhase phase, size_t processed, size_t total,
                                            const char* message) {
  const FirmwareUpdatePhase oldPhase = firmwareStatus.phase;
  firmwareStatus.phase = phase;
  firmwareStatus.processed = processed;
  firmwareStatus.total = total;
  if (message) {
    firmwareStatus.message = message;
  }

  int pct = -1;
  if (total > 0) {
    pct = static_cast<int>((static_cast<uint64_t>(processed) * 100) / total);
  }

  const bool phaseChanged = pct < 0 || lastFirmwareNotifyPercent < 0 || phase != oldPhase;
  const bool pctChangedEnough = pct >= 0 && (pct == 100 || pct >= lastFirmwareNotifyPercent + 2);
  if (firmwareProgressCallback && (phaseChanged || pctChangedEnough)) {
    lastFirmwareNotifyPercent = pct;
    firmwareProgressCallback();
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  const unsigned long sendStart = millis();
  const String uri = server->uri();
  LOG_DBG("WEB", "[HTTP] %s HTML tx begin: bytes=%zu free=%d", uri.c_str(), len, ESP.getFreeHeap());
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
  LOG_DBG("WEB", "[HTTP] %s HTML tx complete: elapsed=%lu ms connected=%d free=%d", uri.c_str(), millis() - sendStart,
          server->client().connected(), ESP.getFreeHeap());
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();                          // Yield to allow WiFi and other tasks to process during long scans
    resetTaskWatchdogIfSubscribed();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleTodosPage() const {
  sendHtmlContent(server.get(), TodoPageHtml, sizeof(TodoPageHtml));
  LOG_DBG("WEB", "Served to-do page");
}

void CrossPointWebServer::handleFileListData() const {
  const unsigned long requestStart = millis();
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "text/plain", "Cannot access protected path");
    return;
  }

  LOG_DBG("WEB", "[HTTP] /api/files begin: path=%s free=%d", currentPath.c_str(), ESP.getFreeHeap());

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  size_t entryCount = 0;
  JsonDocument doc;

  scanFiles(currentPath.c_str(),
            [this, &output, &doc, &seenFirst, &entryCount, requestStart](const FileInfo& info) mutable {
              doc.clear();
              doc["name"] = info.name;
              doc["size"] = info.size;
              doc["isDirectory"] = info.isDirectory;
              doc["isEpub"] = info.isEpub;

              const size_t written = serializeJson(doc, output, outputSize);
              if (written >= outputSize) {
                // JSON output truncated; skip this entry to avoid sending malformed JSON
                LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
                return;
              }

              ++entryCount;
              const unsigned long sendStart = millis();
              if ((entryCount & 0x0F) == 1) {
                LOG_DBG("WEB", "[HTTP] /api/files sending entry=%zu name=%s bytes=%zu elapsed=%lu ms", entryCount,
                        info.name.c_str(), written, millis() - requestStart);
              }
              if (seenFirst) {
                server->sendContent(",");
              } else {
                seenFirst = true;
              }
              server->sendContent(output);
              const unsigned long sendDuration = millis() - sendStart;
              if (sendDuration > 100) {
                LOG_DBG("WEB", "[HTTP] /api/files slow send: entry=%zu duration=%lu ms connected=%d free=%d",
                        entryCount, sendDuration, server->client().connected(), ESP.getFreeHeap());
              }
            });
  LOG_DBG("WEB", "[HTTP] /api/files tail tx begin: entries=%zu elapsed=%lu ms", entryCount, millis() - requestStart);
  server->sendContent("]");
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "[HTTP] /api/files complete: path=%s entries=%zu elapsed=%lu ms free=%d", currentPath.c_str(),
          entryCount, millis() - requestStart, ESP.getFreeHeap());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (itemName.equals(item)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      resetTaskWatchdogIfSubscribed();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    resetTaskWatchdogIfSubscribed();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.get(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    resetTaskWatchdogIfSubscribed();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  resetTaskWatchdogIfSubscribed();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    resetTaskWatchdogIfSubscribed();

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    if (!state.buffer) {
      state.error = "Not enough memory for upload buffer";
      return;
    }

    if (!FsHelpers::isSafePathComponent(state.fileName)) {
      state.error = "Invalid file name";
      LOG_DBG("WEB", "[UPLOAD] Rejected unsafe filename: %s", state.fileName.c_str());
      return;
    }

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Cannot upload to protected path";
      return;
    }

    // Check if file already exists - SD operations can be slow
    resetTaskWatchdogIfSubscribed();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      resetTaskWatchdogIfSubscribed();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    resetTaskWatchdogIfSubscribed();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    resetTaskWatchdogIfSubscribed();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (dropUploadIfCancelled()) return;
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.get() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!server->client().connected()) return;
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        invalidateBookCache(filePath.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }
  if (!FsHelpers::isSafePathComponent(folderName)) {
    LOG_DBG("WEB", "Rejected unsafe folder name: %s", folderName.c_str());
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }
  if (isProtectedItemName(folderName)) {
    LOG_DBG("WEB", "Rejected protected folder name: %s", folderName.c_str());
    server->send(403, "text/plain", "Cannot create protected item");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (isProtectedPath(folderPath)) {
    server->send(403, "text/plain", "Cannot create protected item");
    return;
  }

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected items or items inside protected folders");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (!FsHelpers::isSafePathComponent(newName)) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile item = Storage.open(itemPath.c_str());
  if (!item) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  const bool isDirectory = item.isDirectory();
  item.close();
  if (containsOpenReaderBook(itemPath)) {
    server->send(409, "text/plain", "Close the currently open book before moving or renaming its file or folder");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to a protected path");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  PathMovePlan plan;
  std::string error;
  if (!preparePathMove(itemPath, newPath, isDirectory, plan, error)) {
    server->send(409, "text/plain", error.c_str());
    return;
  }
  if (!applyPathArtifacts(plan, error)) {
    server->send(500, "text/plain", error.c_str());
    return;
  }

  const bool success = Storage.rename(itemPath.c_str(), newPath.c_str());

  if (success) {
    const bool metadataSaved = updatePathReferences(itemPath, newPath);
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain",
                 metadataSaved ? "Renamed successfully" : "Renamed; some history metadata could not be saved");
  } else {
    rollbackPathArtifacts(plan);
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }
  if (isProtectedPath(itemPath) || isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move protected items or use a protected destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile item = Storage.open(itemPath.c_str());
  if (!item) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  const bool isDirectory = item.isDirectory();
  item.close();
  if (containsOpenReaderBook(itemPath)) {
    server->send(409, "text/plain", "Close the currently open book before moving or renaming its file or folder");
    return;
  }

  if (isDirectory && isPathAtOrBelow(destPath, itemPath)) {
    server->send(400, "text/plain", "Cannot move a folder into itself or one of its subfolders");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath.equalsIgnoreCase(itemPath)) {
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  PathMovePlan plan;
  std::string error;
  if (!preparePathMove(itemPath, newPath, isDirectory, plan, error)) {
    server->send(409, "text/plain", error.c_str());
    return;
  }
  if (!applyPathArtifacts(plan, error)) {
    server->send(500, "text/plain", error.c_str());
    return;
  }

  const bool success = Storage.rename(itemPath.c_str(), newPath.c_str());

  if (success) {
    const bool metadataSaved = updatePathReferences(itemPath, newPath);
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain",
                 metadataSaved ? "Moved successfully" : "Moved; some history metadata could not be saved");
  } else {
    rollbackPathArtifacts(plan);
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files are protected
    if (isProtectedPath(itemPath)) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (const auto* item : HIDDEN_ITEMS) {
      if (itemName.equals(item)) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      HalFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  const unsigned long requestStart = millis();
  LOG_DBG("WEB", "[HTTP] /api/settings begin: free=%d", ESP.getFreeHeap());

  // Pass the SD font registry so the fontFamily setting's enumStringValues
  // includes SD-resident families — otherwise the web API only exposes the
  // built-in fonts plus any SD-card families.
  const auto& settings = getSettingsList(&sdFontSystem.registry());
  LOG_DBG("WEB", "[HTTP] /api/settings data ready: count=%zu elapsed=%lu ms free=%d", settings.size(),
          millis() - requestStart, ESP.getFreeHeap());

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  LOG_DBG("WEB", "[HTTP] /api/settings headers tx begin: elapsed=%lu ms", millis() - requestStart);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  size_t itemIndex = 0;
  size_t itemCount = 0;
  JsonDocument doc;

  for (const auto& s : settings) {
    ++itemIndex;
    if (!s.key) continue;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          const uint8_t value = s.nameId == StrId::STR_FONT_FAMILY
                                    ? CrossPointSettings::normalizeBuiltinFontFamily(SETTINGS.*(s.valuePtr))
                                    : SETTINGS.*(s.valuePtr);
          doc["value"] = static_cast<int>(value);
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        if (!doc[s.key].is<int>()) break;
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    const unsigned long sendStart = millis();
    LOG_DBG("WEB", "[HTTP] /api/settings tx begin: item=%zu key=%s bytes=%zu", itemIndex, s.key, written);
    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
    ++itemCount;
    const unsigned long sendDuration = millis() - sendStart;
    if (sendDuration > 100) {
      LOG_DBG("WEB", "[HTTP] /api/settings slow send: item=%zu key=%s duration=%lu ms connected=%d free=%d", itemIndex,
              s.key, sendDuration, server->client().connected(), ESP.getFreeHeap());
    }
    yield();                          // Let WiFi and other tasks run during slow responses.
    resetTaskWatchdogIfSubscribed();  // sendContent() can block on the network client.
  }

  LOG_DBG("WEB", "[HTTP] /api/settings tail tx begin: items=%zu elapsed=%lu ms", itemCount, millis() - requestStart);
  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "[HTTP] /api/settings complete: items=%zu elapsed=%lu ms free=%d", itemCount, millis() - requestStart,
          ESP.getFreeHeap());
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          const bool metadataChanged =
              s.valuePtr == &CrossPointSettings::libraryUseMetadata && SETTINGS.*(s.valuePtr) != val;
          SETTINGS.*(s.valuePtr) = val;
          if (metadataChanged) library::markLibraryIndexDirty();
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max &&
            (s.valueRange.step == 0 || (val - s.valueRange.min) % s.valueRange.step == 0)) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

void CrossPointWebServer::handleGetNutstoreConfig() const {
  const unsigned long requestStart = millis();
  LOG_DBG("WEB", "[HTTP] /api/nutstore/config begin: free=%d", ESP.getFreeHeap());
  NUTSTORE_CONFIG.loadFromFile();
  const auto& cfg = NUTSTORE_CONFIG.get();
  JsonDocument doc;
  doc["enabled"] = cfg.enabled;
  doc["baseUrl"] = cfg.baseUrl;
  doc["username"] = cfg.username;
  doc["password"] = "";
  doc["remotePath"] = cfg.remotePath;
  doc["localPath"] = cfg.localPath;
  doc["recursive"] = true;
  doc["mirrorDelete"] = true;

  String json;
  serializeJson(doc, json);
  LOG_DBG("WEB", "[HTTP] /api/nutstore/config tx begin: bytes=%zu elapsed=%lu ms", json.length(),
          millis() - requestStart);
  server->send(200, "application/json", json);
  LOG_DBG("WEB", "[HTTP] /api/nutstore/config complete: elapsed=%lu ms free=%d", millis() - requestStart,
          ESP.getFreeHeap());
}

void CrossPointWebServer::handlePostNutstoreConfig() {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"Missing JSON body\"}");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const bool hasLocalPath = !doc["localPath"].isNull();
  std::string localPath;
  if (hasLocalPath) {
    if (!doc["localPath"].is<const char*>()) {
      server->send(400, "application/json", "{\"error\":\"Local Path must be a string\"}");
      return;
    }
    String requestedLocalPath = doc["localPath"].as<String>();
    requestedLocalPath.trim();
    if (!NutstoreConfigStore::normalizeLocalPath(requestedLocalPath.c_str(), localPath)) {
      server->send(400, "application/json",
                   "{\"error\":\"Local Path must be a safe absolute SD-card directory (max 128 bytes)\"}");
      return;
    }
  }

  NUTSTORE_CONFIG.loadFromFile();
  auto& cfg = NUTSTORE_CONFIG.mutableConfig();
  cfg.enabled = doc["enabled"] | cfg.enabled;
  cfg.baseUrl = doc["baseUrl"] | cfg.baseUrl;
  cfg.username = doc["username"] | cfg.username;
  const std::string password = doc["password"] | std::string("");
  if (!password.empty()) cfg.password = password;
  cfg.remotePath = doc["remotePath"] | cfg.remotePath;
  if (hasLocalPath) cfg.localPath = localPath;
  cfg.recursive = true;
  cfg.mirrorDelete = true;

  if (!NUTSTORE_CONFIG.saveToFile()) {
    server->send(500, "application/json", "{\"error\":\"Failed to save Nutstore config\"}");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["localPath"] = cfg.localPath;
  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handlePostNutstoreSync() {
  NUTSTORE_CONFIG.loadFromFile();
  bool cancel = false;
  const bool ok = NutstoreSync::run(
      NUTSTORE_CONFIG.get(), nutstoreStatus, [this](const NutstoreSyncStatus& s) { nutstoreStatus = s; }, &cancel);

  JsonDocument doc;
  doc["ok"] = ok;
  doc["phase"] = NutstoreSync::phaseName(nutstoreStatus.phase);
  doc["message"] = nutstoreStatus.message;
  doc["downloaded"] = nutstoreStatus.downloaded;
  doc["skipped"] = nutstoreStatus.skipped;
  doc["deleted"] = nutstoreStatus.deleted;
  String json;
  serializeJson(doc, json);
  server->send(ok ? 200 : 400, "application/json", json);
}

void CrossPointWebServer::handleGetNutstoreStatus() const {
  JsonDocument doc;
  doc["phase"] = NutstoreSync::phaseName(nutstoreStatus.phase);
  doc["processed"] = static_cast<unsigned long>(nutstoreStatus.processed);
  doc["total"] = static_cast<unsigned long>(nutstoreStatus.total);
  doc["downloaded"] = static_cast<unsigned long>(nutstoreStatus.downloaded);
  doc["skipped"] = static_cast<unsigned long>(nutstoreStatus.skipped);
  doc["deleted"] = static_cast<unsigned long>(nutstoreStatus.deleted);
  doc["currentFile"] = nutstoreStatus.currentFile;
  doc["message"] = nutstoreStatus.message;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleGetTodos() const {
  std::vector<TodoItem> items;
  if (!TODO_STORE.getItems(items)) {
    server->send(500, "application/json", "{\"error\":\"Could not load to-do list\"}");
    return;
  }

  JsonDocument doc;
  doc["maxItems"] = TodoStore::MAX_ITEMS;
  JsonArray jsonItems = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject entry = jsonItems.add<JsonObject>();
    entry["id"] = item.id;
    entry["title"] = item.title;
    entry["scheduledAt"] = item.scheduledAt;
    entry["completed"] = item.completed;
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handlePostTodo() {
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain")) || !doc["title"].is<const char*>() ||
      !doc["scheduledAt"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid to-do item\"}");
    return;
  }

  String title = doc["title"].as<String>();
  const std::string scheduledAt = doc["scheduledAt"].as<std::string>();
  title.trim();
  if (title.isEmpty() || title.length() > TodoStore::MAX_TITLE_BYTES) {
    server->send(400, "application/json", "{\"error\":\"Title must contain 1-120 bytes\"}");
    return;
  }
  if (!TodoStore::isValidScheduledAt(scheduledAt)) {
    server->send(400, "application/json", "{\"error\":\"Enter a valid date and time\"}");
    return;
  }

  std::vector<TodoItem> items;
  if (!TODO_STORE.getItems(items)) {
    server->send(500, "application/json", "{\"error\":\"Could not load to-do list\"}");
    return;
  }
  if (items.size() >= TodoStore::MAX_ITEMS) {
    server->send(400, "application/json", "{\"error\":\"To-do list is full\"}");
    return;
  }

  TodoItem item;
  if (!TODO_STORE.add(title.c_str(), scheduledAt, item)) {
    server->send(500, "application/json", "{\"error\":\"Could not save to-do item\"}");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["id"] = item.id;
  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleToggleTodo() {
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain"))) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }
  const uint32_t id = doc["id"] | 0U;
  if (id == 0) {
    server->send(400, "application/json", "{\"error\":\"Invalid to-do id\"}");
    return;
  }

  std::vector<TodoItem> items;
  if (!TODO_STORE.getItems(items)) {
    server->send(500, "application/json", "{\"error\":\"Could not load to-do list\"}");
    return;
  }
  if (std::none_of(items.begin(), items.end(), [id](const TodoItem& item) { return item.id == id; })) {
    server->send(404, "application/json", "{\"error\":\"To-do item not found\"}");
    return;
  }

  TodoItem item;
  if (!TODO_STORE.toggle(id, item)) {
    server->send(500, "application/json", "{\"error\":\"Could not save to-do item\"}");
    return;
  }
  JsonDocument response;
  response["ok"] = true;
  response["id"] = item.id;
  response["completed"] = item.completed;
  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleDeleteTodo() {
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain"))) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }
  const uint32_t id = doc["id"] | 0U;
  if (id == 0) {
    server->send(400, "application/json", "{\"error\":\"Invalid to-do id\"}");
    return;
  }

  std::vector<TodoItem> items;
  if (!TODO_STORE.getItems(items)) {
    server->send(500, "application/json", "{\"error\":\"Could not load to-do list\"}");
    return;
  }
  if (std::none_of(items.begin(), items.end(), [id](const TodoItem& item) { return item.id == id; })) {
    server->send(404, "application/json", "{\"error\":\"To-do item not found\"}");
    return;
  }
  if (!TODO_STORE.remove(id)) {
    server->send(500, "application/json", "{\"error\":\"Could not save to-do item\"}");
    return;
  }
  server->send(200, "application/json", "{\"ok\":true}");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const unsigned long requestStart = millis();
  LOG_DBG("WEB", "[HTTP] /api/opds begin: free=%d", ESP.getFreeHeap());
  const auto& servers = OPDS_STORE.getServers();
  LOG_DBG("WEB", "[HTTP] /api/opds data ready: count=%zu elapsed=%lu ms free=%d", servers.size(),
          millis() - requestStart, ESP.getFreeHeap());

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  LOG_DBG("WEB", "[HTTP] /api/opds headers tx begin: elapsed=%lu ms", millis() - requestStart);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    const unsigned long sendStart = millis();
    LOG_DBG("WEB", "[HTTP] /api/opds tx begin: item=%zu bytes=%zu", i, written);
    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    const unsigned long sendDuration = millis() - sendStart;
    if (sendDuration > 100) {
      LOG_DBG("WEB", "[HTTP] /api/opds slow send: item=%zu duration=%lu ms connected=%d free=%d", i, sendDuration,
              server->client().connected(), ESP.getFreeHeap());
    }
    yield();                          // Let WiFi and other tasks run during slow responses.
    resetTaskWatchdogIfSubscribed();  // sendContent() can block on the network client.
  }

  LOG_DBG("WEB", "[HTTP] /api/opds tail tx begin: elapsed=%lu ms", millis() - requestStart);
  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "[HTTP] /api/opds complete: count=%zu elapsed=%lu ms free=%d", servers.size(), millis() - requestStart,
          ESP.getFreeHeap());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const unsigned long requestStart = millis();
  LOG_DBG("WEB", "[HTTP] /api/wifi begin: free=%d", ESP.getFreeHeap());
  const auto summaries = WIFI_STORE.getCredentialSummaries();
  LOG_DBG("WEB", "[HTTP] /api/wifi data ready: count=%zu elapsed=%lu ms free=%d", summaries.size(),
          millis() - requestStart, ESP.getFreeHeap());

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  LOG_DBG("WEB", "[HTTP] /api/wifi headers tx begin: elapsed=%lu ms", millis() - requestStart);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < summaries.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = summaries[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = summaries[i].hasPassword;
    doc["isLastConnected"] = summaries[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    const unsigned long sendStart = millis();
    LOG_DBG("WEB", "[HTTP] /api/wifi tx begin: item=%zu bytes=%zu", i, written);
    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    const unsigned long sendDuration = millis() - sendStart;
    if (sendDuration > 100) {
      LOG_DBG("WEB", "[HTTP] /api/wifi slow send: item=%zu duration=%lu ms connected=%d free=%d", i, sendDuration,
              server->client().connected(), ESP.getFreeHeap());
    }
    yield();                          // Let WiFi and other tasks run during slow responses.
    resetTaskWatchdogIfSubscribed();  // sendContent() can block on the network client.
  }

  LOG_DBG("WEB", "[HTTP] /api/wifi tail tx begin: elapsed=%lu ms", millis() - requestStart);
  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "[HTTP] /api/wifi complete: count=%zu elapsed=%lu ms free=%d", summaries.size(),
          millis() - requestStart, ESP.getFreeHeap());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credential->ssid;
    if (!hasPasswordField) {
      password = credential->password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
  if (!credential) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const std::string ssid = credential->ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          if (!FsHelpers::isSafePathComponent(wsUploadFileName)) {
            LOG_DBG("WS", "START rejected: invalid filename '%s'", wsUploadFileName.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid file name");
            return;
          }
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Cannot upload to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          resetTaskWatchdogIfSubscribed();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          // Open file for writing
          resetTaskWatchdogIfSubscribed();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          resetTaskWatchdogIfSubscribed();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            invalidateBookCache(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      resetTaskWatchdogIfSubscribed();
      size_t written = wsUploadFile.write(payload, length);
      resetTaskWatchdogIfSubscribed();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        invalidateBookCache(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

void CrossPointWebServer::handleFirmwarePage() const {
  sendHtmlContent(server.get(), FirmwarePageHtml, sizeof(FirmwarePageHtml));
  LOG_DBG("WEB", "Served firmware page");
}

void CrossPointWebServer::handleFirmwareUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      resetTaskWatchdogIfSubscribed();
      firmwareUpload.file = HalFile();
      firmwareUpload.filePath = WEB_FIRMWARE_PATH;
      firmwareUpload.valid = false;
      firmwareUpload.magicChecked = false;
      firmwareUpload.bytesWritten = 0;
      firmwareUpload.bufferPos = 0;
      lastFirmwareNotifyPercent = -1;
      setFirmwareStatus(FirmwareUpdatePhase::UPLOADING, 0, upload.totalSize, "Uploading firmware...");

      if (!firmwareUpload.buffer) {
        setFirmwareStatus(FirmwareUpdatePhase::FAILED, 0, upload.totalSize, "Not enough memory for upload buffer.");
        break;
      }

      String filename = upload.filename;
      filename.toLowerCase();
      if (!filename.endsWith(".bin")) {
        LOG_ERR("WEB", "Invalid firmware filename: %s", upload.filename.c_str());
        setFirmwareStatus(FirmwareUpdatePhase::FAILED, 0, upload.totalSize, "Invalid firmware filename.");
        break;
      }

      Storage.mkdir("/.crosspoint");
      if (Storage.exists(WEB_FIRMWARE_PATH)) {
        Storage.remove(WEB_FIRMWARE_PATH);
      }
      if (!Storage.openFileForWrite("WEBFW", WEB_FIRMWARE_PATH, firmwareUpload.file)) {
        LOG_ERR("WEB", "Failed to open firmware upload file: %s", WEB_FIRMWARE_PATH);
        setFirmwareStatus(FirmwareUpdatePhase::FAILED, 0, upload.totalSize, "Could not create firmware upload file.");
        break;
      }

      firmwareUpload.valid = true;
      LOG_INF("WEB", "Firmware upload started: %s", upload.filename.c_str());
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!firmwareUpload.valid) break;
      resetTaskWatchdogIfSubscribed();

      if (!firmwareUpload.magicChecked && upload.currentSize > 0) {
        if (upload.buf[0] != ESP_IMAGE_MAGIC) {
          LOG_ERR("WEB", "Invalid firmware magic: 0x%02X", upload.buf[0]);
          firmwareUpload.valid = false;
          setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, upload.totalSize,
                            "Firmware is not a valid ESP32 image.");
          break;
        }
        firmwareUpload.magicChecked = true;
      }

      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FirmwareUploadState::BUFFER_SIZE - firmwareUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(firmwareUpload.buffer.get() + firmwareUpload.bufferPos, src, chunk);
        firmwareUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (firmwareUpload.bufferPos >= FirmwareUploadState::BUFFER_SIZE) {
          if (firmwareUpload.file.write(firmwareUpload.buffer.get(), firmwareUpload.bufferPos) !=
              firmwareUpload.bufferPos) {
            firmwareUpload.valid = false;
            setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, upload.totalSize,
                              "SD write failed.");
            break;
          }
          firmwareUpload.bytesWritten += firmwareUpload.bufferPos;
          firmwareUpload.bufferPos = 0;
          setFirmwareStatus(FirmwareUpdatePhase::UPLOADING, firmwareUpload.bytesWritten, upload.totalSize,
                            "Uploading firmware...");
          resetTaskWatchdogIfSubscribed();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      if (firmwareUpload.valid && firmwareUpload.bufferPos > 0) {
        if (firmwareUpload.file.write(firmwareUpload.buffer.get(), firmwareUpload.bufferPos) !=
            firmwareUpload.bufferPos) {
          firmwareUpload.valid = false;
          setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, upload.totalSize,
                            "SD write failed.");
        } else {
          firmwareUpload.bytesWritten += firmwareUpload.bufferPos;
        }
        firmwareUpload.bufferPos = 0;
      }
      if (firmwareUpload.valid) {
        setFirmwareStatus(FirmwareUpdatePhase::VALIDATING, firmwareUpload.bytesWritten, firmwareUpload.bytesWritten,
                          "Validating firmware...");
      }
      if (firmwareUpload.file.isOpen()) {
        if (!firmwareUpload.file.close() && firmwareUpload.valid) {
          firmwareUpload.valid = false;
          setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, upload.totalSize,
                            "SD close failed.");
        }
      }

      if (!firmwareUpload.valid && !firmwareUpload.filePath.empty()) {
        Storage.remove(firmwareUpload.filePath.c_str());
      }

      LOG_INF("WEB", "Firmware upload end: valid=%d, %u bytes", firmwareUpload.valid,
              static_cast<unsigned>(firmwareUpload.bytesWritten));
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      if (firmwareUpload.file) {
        firmwareUpload.file.close();
      }
      if (!firmwareUpload.filePath.empty()) {
        Storage.remove(firmwareUpload.filePath.c_str());
      }
      firmwareUpload.valid = false;
      setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, firmwareUpload.bytesWritten,
                        "Firmware upload was aborted.");
      LOG_DBG("WEB", "Firmware upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFirmwareUpload() {
  if (!firmwareUpload.valid || firmwareUpload.bytesWritten == 0) {
    Storage.remove(WEB_FIRMWARE_PATH);
    setFirmwareStatus(FirmwareUpdatePhase::FAILED, firmwareUpload.bytesWritten, firmwareUpload.bytesWritten,
                      "Invalid firmware upload.");
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid firmware upload\"}");
    return;
  }

  LOG_INF("WEB", "Installing uploaded firmware: %u bytes", static_cast<unsigned>(firmwareUpload.bytesWritten));
  setFirmwareStatus(FirmwareUpdatePhase::VALIDATING, 0, firmwareUpload.bytesWritten, "Validating firmware...");

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<CrossPointWebServer*>(ctx);
    self->setFirmwareStatus(FirmwareUpdatePhase::FLASHING, written, total, "Installing firmware...");
  };
  const firmware_flash::Result result = firmware_flash::flashFromSdPath(WEB_FIRMWARE_PATH, progressCb, this);
  Storage.remove(WEB_FIRMWARE_PATH);

  JsonDocument doc;
  doc["ok"] = result == firmware_flash::Result::OK;
  doc["result"] = firmware_flash::resultName(result);
  doc["message"] = firmwareFlashResultMessage(result);

  String json;
  serializeJson(doc, json);

  if (result == firmware_flash::Result::OK) {
    setFirmwareStatus(FirmwareUpdatePhase::SUCCESS, firmwareUpload.bytesWritten, firmwareUpload.bytesWritten,
                      firmwareFlashResultMessage(result));
    firmwareRestartPending = true;
    firmwareRestartAt = millis() + 1500;
    server->send(200, "application/json", json);
  } else {
    setFirmwareStatus(FirmwareUpdatePhase::FAILED, 0, firmwareUpload.bytesWritten, firmwareFlashResultMessage(result));
    server->send(400, "application/json", json);
  }
}
