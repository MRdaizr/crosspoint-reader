#include "PluginPermissions.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cstdio>

#include "PluginHttp.h"
#include "PluginLocations.h"
#include "util/TaskWatchdog.h"

namespace {
std::atomic<bool> systemGate{true};
struct DigestWork {
  mbedtls_sha256_context sha;
  uint8_t buffer[256];
  uint8_t digest[32];
  DigestWork() { mbedtls_sha256_init(&sha); }
  ~DigestWork() { mbedtls_sha256_free(&sha); }
};
bool fingerprint(const char* name, char (&hex)[65]) {
  const auto dir = PluginLocations::findPluginDir(name);
  if (dir.empty()) return false;
  // One checked allocation per permission check keeps SHA/file buffers off the
  // small ESP task stack. Reader open/exit and event hooks do allocate/check
  // here, but this is not performed on every page render.
  auto work = makeUniqueNoThrow<DigestWork>();
  if (!work) {
    LOG_ERR("PPER", "OOM: digest");
    return false;
  }
  if (mbedtls_sha256_starts(&work->sha, 0) != 0 ||
      mbedtls_sha256_update(&work->sha, reinterpret_cast<const uint8_t*>(dir.data()), dir.size()) != 0)
    return false;
  static constexpr const char* FILES[] = {"plugin.js", "main.js", "device.json", "manifest.json"};
  bool marker = false;
  for (const char* file : FILES) {
    const std::string path = dir + "/" + file;
    HalFile f = Storage.open(path.c_str());
    const uint8_t exists = f && !f.isDirectory() ? 1 : 0;
    if (Storage.exists(path.c_str()) && !exists) return false;
    if (mbedtls_sha256_update(&work->sha, reinterpret_cast<const uint8_t*>(file), strlen(file) + 1) != 0 ||
        mbedtls_sha256_update(&work->sha, &exists, 1) != 0)
      return false;
    if (!exists) continue;
    marker = true;
    const uint64_t length = f.fileSize64();
    if (length > (strstr(file, ".js") ? 256u * 1024 : 8u * 1024)) return false;
    if (mbedtls_sha256_update(&work->sha, reinterpret_cast<const uint8_t*>(&length), sizeof(length)) != 0) return false;
    size_t remaining = length;
    size_t yieldedBytes = 0;
    while (remaining) {
      const size_t n = std::min(remaining, sizeof(work->buffer));
      if (f.read(work->buffer, n) != static_cast<int>(n) || mbedtls_sha256_update(&work->sha, work->buffer, n) != 0)
        return false;
      remaining -= n;
      yieldedBytes += n;
      if (yieldedBytes >= 1024 || !remaining) {
        resetTaskWatchdogIfSubscribed();
        vTaskDelay(1);
        yieldedBytes = 0;
      }
    }
  }
  if (!marker || mbedtls_sha256_finish(&work->sha, work->digest) != 0) return false;
  for (size_t i = 0; i < sizeof(work->digest); ++i) snprintf(hex + i * 2, 3, "%02x", work->digest[i]);
  return true;
}
std::string recordPath(const char* name) {
  std::string folded = name;
  for (char& c : folded)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return std::string(PluginPermissions::STORE_DIR) + "/" + folded + ".json";
}
}  // namespace
namespace PluginPermissions {
void setSystemEnabled(bool enabled) { systemGate = enabled; }
bool systemEnabled() { return systemGate; }
Status inspect(const char* plugin) {
  Status status;
  if (!PluginLocations::validName(plugin)) return status;
  std::string raw;
  JsonDocument doc;
  if (pluginhttp::readFile(recordPath(plugin), 256, raw) && deserializeJson(doc, raw) == DeserializationError::Ok)
    status.enabled = doc["enabled"] | false;
  if (!fingerprint(plugin, status.digest)) return status;
  status.available = true;
  status.approved = approvalMatches(status.enabled, doc["digest"] | "", status.digest);
  return status;
}
bool allowed(const char* plugin) { return systemGate && inspect(plugin).approved; }
bool setEnabled(const char* plugin, bool enabled, const char* expectedDigest) {
  if (!PluginLocations::validName(plugin)) return false;
  const auto status = inspect(plugin);
  if (enabled && (!status.available || !expectedDigest || strcmp(status.digest, expectedDigest) != 0)) return false;
  if (status.enabled == enabled && (!enabled || status.approved)) return true;
  if (!Storage.ensureDirectoryExists(STORE_DIR)) return false;
  const auto path = recordPath(plugin);
  const auto tmp = path + ".tmp";
  JsonDocument doc;
  doc["enabled"] = enabled;
  doc["digest"] = enabled ? status.digest : "";
  std::string raw;
  raw.reserve(128);
  serializeJson(doc, raw);
  if (!pluginhttp::writeStaged(tmp, raw.data(), raw.size())) return false;
  return pluginhttp::replaceFile(tmp, path);
}
}  // namespace PluginPermissions
