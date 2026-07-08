#include "NutstoreSync.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <ctime>

#include "NutstoreWebDavClient.h"

namespace {
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

void collectLocalFiles(const std::string& dir, const std::string& root, std::vector<std::string>& relFiles) {
  HalFile d = Storage.open(dir.c_str());
  if (!d || !d.isDirectory()) return;
  for (HalFile f = d.openNextFile(); f; f = d.openNextFile()) {
    char name[256] = {};
    f.getName(name, sizeof(name));
    std::string full = joinPath(dir, name);
    if (f.isDirectory()) {
      collectLocalFiles(full, root, relFiles);
    } else {
      if (full.rfind(root + "/", 0) == 0) {
        relFiles.push_back(full.substr(root.size() + 1));
      }
    }
  }
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

  NutstoreWebDavClient client(config.baseUrl, config.username, config.password);
  std::vector<NutstoreRemoteEntry> remoteEntries;
  std::string error;
  if (!client.listRecursive(config.remotePath, remoteEntries, error)) {
    status.phase = NutstoreSyncPhase::FAILED;
    status.message = error;
    notify(status, callback);
    return false;
  }

  std::vector<NutstoreRemoteEntry> files;
  std::set<std::string> remoteRelFiles;
  for (auto& e : remoteEntries) {
    if (e.isDirectory || e.relativePath.empty()) continue;
    if (!isAllowedReadingFile(e.relativePath)) continue;
    files.push_back(e);
    remoteRelFiles.insert(e.relativePath);
  }

  status.phase = NutstoreSyncPhase::DOWNLOADING;
  status.total = files.size();
  status.processed = 0;
  status.message = "Downloading Nutstore files...";
  notify(status, callback);

  for (const auto& entry : files) {
    if (cancelFlag && *cancelFlag) {
      status.phase = NutstoreSyncPhase::CANCELLED;
      status.message = "Sync cancelled";
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
    std::vector<std::string> localFiles;
    collectLocalFiles("/Nutstore", "/Nutstore", localFiles);
    status.total = localFiles.size();
    notify(status, callback);

    for (const auto& rel : localFiles) {
      if (cancelFlag && *cancelFlag) {
        status.phase = NutstoreSyncPhase::CANCELLED;
        status.message = "Sync cancelled";
        notify(status, callback);
        return false;
      }
      status.currentFile = rel;
      if (isAllowedReadingFile(rel) && remoteRelFiles.find(rel) == remoteRelFiles.end()) {
        std::string localPath;
        if (localPathForRelative(rel, localPath) && Storage.remove(localPath.c_str())) {
          status.deleted++;
        }
      }
      status.processed++;
      notify(status, callback);
    }
    removeEmptyDirs("/Nutstore");
  }

  status.phase = NutstoreSyncPhase::SUCCESS;
  status.message = "Nutstore sync complete";
  notify(status, callback);
  return true;
}
