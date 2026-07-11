#include "NutstoreSync.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "NutstoreWebDavClient.h"

namespace {
constexpr const char* REMOTE_MANIFEST_PATH = "/Nutstore/.nutstore-manifest.tmp";

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (b.empty()) return a;
  if (a.empty() || a == "/") return "/" + b;
  return a + "/" + b;
}

bool isProtectedLocalRoot(const std::string& path) {
  return path == "/Nutstore";
}

bool localPathForRelative(const std::string& rel, std::string& out) {
  if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) return false;
  out = joinPath("/Nutstore", rel);
  std::string normalized = FsHelpers::normalisePath(out.c_str());
  if (!normalized.empty() && normalized.front() != '/') normalized.insert(normalized.begin(), '/');
  if (normalized.rfind("/Nutstore/", 0) != 0 && normalized != "/Nutstore") return false;
  out = normalized;
  return true;
}

void notify(NutstoreSyncStatus& status, const NutstoreSync::StatusCallback& cb) {
  if (cb) cb(status);
}

bool systemTimeLooksValid() {
  time_t now = time(nullptr);
  struct tm timeinfo = {};
  gmtime_r(&now, &timeinfo);
  return timeinfo.tm_year >= 125;  // 2025 or later; enough for current TLS certificate validation.
}

bool ensureSystemTime() {
  LOG_INF("NUT", "Syncing system time before WebDAV TLS connection...");

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.aliyun.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_setservername(2, "time.nist.gov");
  esp_sntp_init();

  for (int i = 0; i < 150; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && systemTimeLooksValid()) {
      time_t now = time(nullptr);
      struct tm timeinfo = {};
      gmtime_r(&now, &timeinfo);
      LOG_INF("NUT", "System time synced for WebDAV TLS: %04d-%02d-%02d %02d:%02d:%02d UTC",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
              timeinfo.tm_sec);
      return true;
    }
    delay(100);
  }

  if (systemTimeLooksValid()) {
    LOG_INF("NUT", "System time looks valid; continuing without fresh SNTP completion");
    return true;
  }

  LOG_ERR("NUT", "System time sync timed out; WebDAV TLS will likely fail");
  return false;
}

bool ensureParentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true;
  return Storage.mkdir(path.substr(0, slash).c_str());
}

bool writeManifestEntry(HalFile& file, const NutstoreRemoteEntry& entry) {
  char size[24] = {};
  const int sizeLength = snprintf(size, sizeof(size), "%u", static_cast<unsigned>(entry.size));
  static constexpr char TAB = '\t';
  static constexpr char NEWLINE = '\n';
  return sizeLength > 0 && file.write(size, sizeLength) == static_cast<size_t>(sizeLength) &&
         file.write(&TAB, 1) == 1 && file.write(entry.href.c_str(), entry.href.size()) == entry.href.size() &&
         file.write(&TAB, 1) == 1 && file.write(entry.relativePath.c_str(), entry.relativePath.size()) == entry.relativePath.size() &&
         file.write(&NEWLINE, 1) == 1;
}

bool readManifestEntry(HalFile& file, NutstoreRemoteEntry& entry) {
  std::string line;
  line.reserve(192);
  while (file.available() > 0) {
    const int ch = file.read();
    if (ch < 0 || ch == '\n') break;
    if (ch != '\r') line.push_back(static_cast<char>(ch));
  }
  const size_t firstTab = line.find('\t');
  const size_t secondTab = firstTab == std::string::npos ? std::string::npos : line.find('\t', firstTab + 1);
  if (firstTab == std::string::npos || secondTab == std::string::npos || secondTab + 1 >= line.size()) return false;
  entry = {};
  entry.size = static_cast<size_t>(strtoull(line.c_str(), nullptr, 10));
  entry.href = line.substr(firstTab + 1, secondTab - firstTab - 1);
  entry.relativePath = line.substr(secondTab + 1);
  return !entry.href.empty() && !entry.relativePath.empty();
}

bool manifestContains(HalFile& manifest, const std::string& relativePath) {
  if (!manifest.seek(0)) return false;
  while (manifest.available() > 0) {
    NutstoreRemoteEntry entry;
    if (!readManifestEntry(manifest, entry)) return false;
    if (entry.relativePath == relativePath) return true;
  }
  return false;
}

size_t countLocalFiles(const std::string& dir) {
  size_t count = 0;
  HalFile d = Storage.open(dir.c_str());
  if (!d || !d.isDirectory()) return 0;
  for (HalFile f = d.openNextFile(); f; f = d.openNextFile()) {
    char name[256] = {};
    f.getName(name, sizeof(name));
    std::string full = joinPath(dir, name);
    if (f.isDirectory()) {
      count += countLocalFiles(full);
    } else {
      ++count;
    }
  }
  return count;
}

bool deleteLocalFilesMissingRemote(const std::string& dir, const std::string& root, HalFile& manifest,
                                   NutstoreSyncStatus& status, const NutstoreSync::StatusCallback& callback,
                                   bool* cancelFlag) {
  HalFile d = Storage.open(dir.c_str());
  if (!d || !d.isDirectory()) return true;
  for (HalFile f = d.openNextFile(); f; f = d.openNextFile()) {
    if (cancelFlag && *cancelFlag) return false;
    char name[256] = {};
    f.getName(name, sizeof(name));
    const std::string full = joinPath(dir, name);
    if (f.isDirectory()) {
      if (!deleteLocalFilesMissingRemote(full, root, manifest, status, callback, cancelFlag)) return false;
      continue;
    }
    if (full.rfind(root + "/", 0) != 0) continue;
    const std::string relativePath = full.substr(root.size() + 1);
    status.currentFile = relativePath;
    if (NutstoreSync::isAllowedReadingFile(relativePath) && !manifestContains(manifest, relativePath)) {
      std::string protectedPath;
      if (localPathForRelative(relativePath, protectedPath) && Storage.remove(protectedPath.c_str())) {
        ++status.deleted;
      }
    }
    ++status.processed;
    notify(status, callback);
  }
  return true;
}

void removeEmptyDirs(const std::string& dir) {
  if (isProtectedLocalRoot(dir)) return;
  HalFile d = Storage.open(dir.c_str());
  if (!d || !d.isDirectory()) return;
  bool empty = true;
  for (HalFile f = d.openNextFile(); f; f = d.openNextFile()) {
    char name[256] = {};
    f.getName(name, sizeof(name));
    std::string child = joinPath(dir, name);
    if (f.isDirectory()) {
      removeEmptyDirs(child);
      if (Storage.exists(child.c_str())) empty = false;
    } else {
      empty = false;
    }
  }
  d.close();
  if (empty) Storage.rmdir(dir.c_str());
}
}  // namespace

const char* NutstoreSync::phaseName(NutstoreSyncPhase phase) {
  switch (phase) {
    case NutstoreSyncPhase::IDLE:
      return "Idle";
    case NutstoreSyncPhase::ENUMERATING:
      return "Enumerating";
    case NutstoreSyncPhase::DOWNLOADING:
      return "Downloading";
    case NutstoreSyncPhase::DELETING:
      return "Deleting";
    case NutstoreSyncPhase::SUCCESS:
      return "Success";
    case NutstoreSyncPhase::FAILED:
      return "Failed";
    case NutstoreSyncPhase::CANCELLED:
      return "Cancelled";
  }
  return "Unknown";
}

bool NutstoreSync::isAllowedReadingFile(const std::string& path) {
  const std::string p = lower(path);
  static constexpr const char* EXT[] = {".epub", ".txt", ".xtc", ".xtch", ".bmp", ".pdf", ".md"};
  for (const char* ext : EXT) {
    if (p.size() >= strlen(ext) && p.compare(p.size() - strlen(ext), strlen(ext), ext) == 0) return true;
  }
  return false;
}

bool NutstoreSync::run(const NutstoreConfig& config, NutstoreSyncStatus& status, StatusCallback callback,
                       bool* cancelFlag) {
  status = {};
  status.phase = NutstoreSyncPhase::ENUMERATING;
  status.message = "Enumerating Nutstore files...";
  notify(status, callback);

  if (!config.enabled) {
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = "Nutstore sync is disabled";
    notify(status, callback);
    return false;
  }
  if (config.username.empty() || config.password.empty()) {
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = "Nutstore credentials are missing";
    notify(status, callback);
    return false;
  }

  status.message = "Syncing clock for HTTPS...";
  notify(status, callback);
  ensureSystemTime();

  Storage.mkdir("/Nutstore");
  Storage.remove(REMOTE_MANIFEST_PATH);

  NutstoreWebDavClient client(config.baseUrl, config.username, config.password);
  HalFile manifest;
  if (!Storage.openFileForWrite("NUT", REMOTE_MANIFEST_PATH, manifest)) {
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = "Could not create Nutstore file list";
    notify(status, callback);
    return false;
  }

  size_t fileCount = 0;
  std::string error;
  const bool listed = client.listRecursive(
      config.remotePath,
      [&manifest, &fileCount](NutstoreRemoteEntry&& entry, std::string& callbackError) {
        if (!NutstoreSync::isAllowedReadingFile(entry.relativePath)) return true;
        if (!writeManifestEntry(manifest, entry)) {
          callbackError = "Could not save Nutstore file list";
          return false;
        }
        ++fileCount;
        return true;
      },
      error);
  manifest.close();
  if (!listed) {
    Storage.remove(REMOTE_MANIFEST_PATH);
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = error;
    notify(status, callback);
    return false;
  }

  if (!Storage.openFileForRead("NUT", REMOTE_MANIFEST_PATH, manifest)) {
    Storage.remove(REMOTE_MANIFEST_PATH);
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = "Could not read Nutstore file list";
    notify(status, callback);
    return false;
  }

  status.phase = NutstoreSyncPhase::DOWNLOADING;
  status.total = fileCount;
  status.processed = 0;
  status.message = "Downloading Nutstore files...";
  notify(status, callback);

  while (manifest.available() > 0) {
    if (cancelFlag && *cancelFlag) {
      manifest.close();
      Storage.remove(REMOTE_MANIFEST_PATH);
      status.phase = NutstoreSyncPhase::CANCELLED;
      status.message = "Sync cancelled";
      notify(status, callback);
      return false;
    }

    NutstoreRemoteEntry entry;
    if (!readManifestEntry(manifest, entry)) {
      manifest.close();
      Storage.remove(REMOTE_MANIFEST_PATH);
      status.phase = NutstoreSyncPhase::FAILED;
      status.message = "Nutstore file list is invalid";
      notify(status, callback);
      return false;
    }
    std::string localPath;
    if (!localPathForRelative(entry.relativePath, localPath)) {
      LOG_ERR("NUT", "Skipping unsafe local path: %s", entry.relativePath.c_str());
      status.skipped++;
      status.processed++;
      continue;
    }
    status.currentFile = entry.relativePath;

    bool needsDownload = true;
    HalFile existing;
    if (Storage.openFileForRead("NUT", localPath.c_str(), existing)) {
      needsDownload = existing.fileSize() != entry.size;
      existing.close();
    }
    LOG_DBG("NUT", "%s %s (%u bytes)", needsDownload ? "Downloading" : "Skipping existing", localPath.c_str(),
            (unsigned)entry.size);

    if (needsDownload) {
      ensureParentDir(localPath);
      std::string dlError;
      if (!client.downloadFile(entry, localPath,
                               [&status, &callback](size_t done, size_t total) {
                                 status.message = "Downloading Nutstore files...";
                                 notify(status, callback);
                               },
                               dlError)) {
        manifest.close();
        Storage.remove(REMOTE_MANIFEST_PATH);
        status.phase = NutstoreSyncPhase::FAILED;
        status.message = dlError;
        notify(status, callback);
        return false;
      }
      status.downloaded++;
    } else {
      status.skipped++;
    }
    status.processed++;
    notify(status, callback);
  }

  if (config.mirrorDelete) {
    status.phase = NutstoreSyncPhase::DELETING;
    status.message = "Deleting local files missing from Nutstore...";
    status.processed = 0;
    status.total = countLocalFiles("/Nutstore");
    notify(status, callback);

    if (!deleteLocalFilesMissingRemote("/Nutstore", "/Nutstore", manifest, status, callback, cancelFlag)) {
      manifest.close();
      Storage.remove(REMOTE_MANIFEST_PATH);
      status.phase = NutstoreSyncPhase::CANCELLED;
      status.message = "Sync cancelled";
      notify(status, callback);
      return false;
    }
    removeEmptyDirs("/Nutstore");
  }

  manifest.close();
  Storage.remove(REMOTE_MANIFEST_PATH);
  status.phase = NutstoreSyncPhase::SUCCESS;
  status.message = "Nutstore sync complete";
  notify(status, callback);
  return true;
}
