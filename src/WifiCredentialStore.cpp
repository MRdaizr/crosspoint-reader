#include "WifiCredentialStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include <algorithm>

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version (for binary migration)
constexpr uint8_t WIFI_FILE_VERSION = 2;

// File paths
constexpr char WIFI_FILE_BIN[] = "/.crosspoint/wifi.bin";
constexpr char WIFI_FILE_JSON[] = "/.crosspoint/wifi.json";
constexpr char WIFI_FILE_BAK[] = "/.crosspoint/wifi.bin.bak";

constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 64;

// Legacy obfuscation key - "CrossPoint" in ASCII (only used for binary migration)
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}

enum class ReadStringResult { OK, TOO_LONG, INVALID };

ReadStringResult readBoundedString(HalFile& file, std::string& out, size_t maxLength, const char* field) {
  uint32_t length = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&length), sizeof(length)) != static_cast<int>(sizeof(length))) {
    LOG_ERR("WCS", "Failed to read %s length from binary file", field);
    return ReadStringResult::INVALID;
  }

  const int available = file.available();
  if (available < 0 || static_cast<uint32_t>(available) < length) {
    LOG_ERR("WCS", "Truncated %s in binary file", field);
    return ReadStringResult::INVALID;
  }

  if (length > maxLength) {
    LOG_ERR("WCS", "Discarding oversized %s in binary file (%u > %u)", field, static_cast<unsigned>(length),
            static_cast<unsigned>(maxLength));
    uint8_t discard[32];
    size_t remaining = length;
    while (remaining > 0) {
      const size_t chunk = std::min(remaining, sizeof(discard));
      if (file.read(discard, chunk) != static_cast<int>(chunk)) {
        LOG_ERR("WCS", "Failed to discard oversized %s", field);
        return ReadStringResult::INVALID;
      }
      remaining -= chunk;
    }
    out.clear();
    return ReadStringResult::TOO_LONG;
  }

  out.assign(length, '\0');
  if (length > 0 && file.read(reinterpret_cast<uint8_t*>(&out[0]), length) != static_cast<int>(length)) {
    LOG_ERR("WCS", "Failed to read %s from binary file", field);
    out.clear();
    return ReadStringResult::INVALID;
  }
  return ReadStringResult::OK;
}
}  // namespace

bool WifiCredentialStore::saveToFileLocked() const {
  // The persistence mutex serializes snapshots and SD writes, while JsonSettingsIO
  // takes only the short-lived state mutex. Never hold stateMutex across this call.
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveWifi(*this, WIFI_FILE_JSON);
}

bool WifiCredentialStore::saveToFile() const {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  return saveToFileLocked();
}

bool WifiCredentialStore::loadFromFile() {
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  if (loaded) return true;

  // Try JSON first
  if (Storage.exists(WIFI_FILE_JSON)) {
    String json = Storage.readFile(WIFI_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      const bool result = JsonSettingsIO::loadWifi(*this, json.c_str(), &resave);
      if (result && resave) {
        LOG_DBG("WCS", "Resaving JSON with validated credential metadata");
        saveToFileLocked();
      }
      loaded = true;
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(WIFI_FILE_BIN)) {
    Snapshot loaded;
    if (loadFromBinaryFile(loaded)) {
      replaceState(std::move(loaded));
      if (saveToFileLocked()) {
        Storage.rename(WIFI_FILE_BIN, WIFI_FILE_BAK);
        LOG_DBG("WCS", "Migrated wifi.bin to wifi.json");
        this->loaded = true;
        return true;
      }
      LOG_ERR("WCS", "Failed to save wifi during migration");
      this->loaded = true;
      return false;
    }
  }

  loaded = true;
  return false;
}

bool WifiCredentialStore::ensureLoaded() const {
  return const_cast<WifiCredentialStore*>(this)->loadFromFile();
}

bool WifiCredentialStore::loadFromBinaryFile(Snapshot& loaded) const {
  HalFile file;
  if (!Storage.openFileForRead("WCS", WIFI_FILE_BIN, file)) {
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, sizeof(version)) != static_cast<int>(sizeof(version))) {
    file.close();
    LOG_ERR("WCS", "Failed to read binary file version");
    return false;
  }
  if (version > WIFI_FILE_VERSION) {
    LOG_DBG("WCS", "Unknown file version: %u", version);
    file.close();
    return false;
  }

  loaded = {};
  if (version >= 2) {
    const auto result = readBoundedString(file, loaded.lastConnectedSsid, MAX_SSID_LENGTH, "lastConnectedSsid");
    if (result == ReadStringResult::INVALID) {
      file.close();
      return false;
    }
    if (result == ReadStringResult::TOO_LONG) loaded.lastConnectedSsid.clear();
  }

  uint8_t count = 0;
  if (file.read(&count, sizeof(count)) != static_cast<int>(sizeof(count))) {
    file.close();
    LOG_ERR("WCS", "Failed to read binary credential count");
    return false;
  }

  loaded.credentials.reserve(std::min<size_t>(count, MAX_NETWORKS));
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;
    const auto ssidResult = readBoundedString(file, cred.ssid, MAX_SSID_LENGTH, "SSID");
    if (ssidResult == ReadStringResult::INVALID) {
      file.close();
      return false;
    }

    const auto passwordResult = readBoundedString(file, cred.password, MAX_PASSWORD_LENGTH, "password");
    if (passwordResult == ReadStringResult::INVALID) {
      file.close();
      return false;
    }

    if (ssidResult == ReadStringResult::TOO_LONG || passwordResult == ReadStringResult::TOO_LONG) {
      LOG_ERR("WCS", "Discarding invalid binary Wi-Fi credential at index %u", static_cast<unsigned>(i));
      continue;
    }

    legacyDeobfuscate(cred.password);
    loaded.credentials.push_back(std::move(cred));
  }

  file.close();
  LOG_DBG("WCS", "Loaded %zu WiFi credentials from binary file", loaded.credentials.size());
  return true;
}

WifiCredentialStore::Snapshot WifiCredentialStore::snapshot() const {
  std::lock_guard<std::mutex> stateLock(stateMutex);
  return Snapshot{credentials, lastConnectedSsid};
}

void WifiCredentialStore::replaceState(Snapshot&& loaded) {
  std::lock_guard<std::mutex> stateLock(stateMutex);
  credentials = std::move(loaded.credentials);
  lastConnectedSsid = std::move(loaded.lastConnectedSsid);
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  ensureLoaded();
  if (password.size() > MAX_PASSWORD_LENGTH) {
    LOG_ERR("WCS", "Rejecting oversized Wi-Fi password (%u > %u)", static_cast<unsigned>(password.size()),
            static_cast<unsigned>(MAX_PASSWORD_LENGTH));
    return false;
  }

  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex);
    const auto cred = std::find_if(credentials.begin(), credentials.end(),
                                   [&ssid](const WifiCredential& item) { return item.ssid == ssid; });
    if (cred != credentials.end()) {
      cred->password = password;
      LOG_DBG("WCS", "Updated credentials for: %s", ssid.c_str());
    } else {
      if (credentials.size() >= MAX_NETWORKS) {
        LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
        return false;
      }
      credentials.push_back({ssid, password});
      LOG_DBG("WCS", "Added credentials for: %s", ssid.c_str());
    }
  }
  return saveToFileLocked();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex);
    const auto cred = std::find_if(credentials.begin(), credentials.end(),
                                   [&ssid](const WifiCredential& item) { return item.ssid == ssid; });
    if (cred == credentials.end()) return false;

    credentials.erase(cred);
    if (ssid == lastConnectedSsid) lastConnectedSsid.clear();
    LOG_DBG("WCS", "Removed credentials for: %s", ssid.c_str());
  }
  return saveToFileLocked();
}

std::optional<WifiCredential> WifiCredentialStore::findCredential(const std::string& ssid) const {
  ensureLoaded();
  std::lock_guard<std::mutex> stateLock(stateMutex);
  const auto cred = std::find_if(credentials.begin(), credentials.end(),
                                 [&ssid](const WifiCredential& item) { return item.ssid == ssid; });
  if (cred == credentials.end()) return std::nullopt;
  return *cred;
}

std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(size_t index) const {
  ensureLoaded();
  std::lock_guard<std::mutex> stateLock(stateMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index];
}

std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  ensureLoaded();
  std::lock_guard<std::mutex> stateLock(stateMutex);
  std::vector<WifiCredentialSummary> summaries;
  summaries.resize(credentials.size());
  std::transform(credentials.begin(), credentials.end(), summaries.begin(), [this](const WifiCredential& credential) {
    return WifiCredentialSummary{credential.ssid, !credential.password.empty(), credential.ssid == lastConnectedSsid};
  });
  return summaries;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const { return findCredential(ssid).has_value(); }

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex);
    if (lastConnectedSsid == ssid) return;
    lastConnectedSsid = ssid;
  }
  saveToFileLocked();
}

std::string WifiCredentialStore::getLastConnectedSsid() const {
  ensureLoaded();
  std::lock_guard<std::mutex> stateLock(stateMutex);
  return lastConnectedSsid;
}

void WifiCredentialStore::clearLastConnectedSsid() {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex);
    if (lastConnectedSsid.empty()) return;
    lastConnectedSsid.clear();
  }
  saveToFileLocked();
}

void WifiCredentialStore::clearAll() {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }
  saveToFileLocked();
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
