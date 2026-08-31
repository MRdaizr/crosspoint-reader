#include "WifiCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version (for binary migration)
constexpr uint8_t WIFI_FILE_VERSION = 2;

// File paths
constexpr char WIFI_FILE_BIN[] = "/.crosspoint/wifi.bin";
constexpr char WIFI_FILE_BAK[] = "/.crosspoint/wifi.bin.bak";

constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 64;
constexpr size_t MAX_PASSWORD_B64_LENGTH = ((MAX_PASSWORD_LENGTH + 2) / 3) * 4;

// Legacy obfuscation key - "CrossPoint" in ASCII (only used for binary migration)
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}

enum class ReadStringResult { OK, TOO_LONG, INVALID };

uint32_t passwordCrc32(const std::string& password) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : password) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return ~crc;
}

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
  // The persistence mutex serializes snapshots and SD writes, while
  // PersistableStore takes only its store mutex. Never hold stateMutex across
  // the JSON serialization or SD write.
  return PersistableStore<WifiCredentialStore>::saveToFile();
}

bool WifiCredentialStore::saveToFile() const {
  ensureLoaded();
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  return saveToFileLocked();
}

bool WifiCredentialStore::loadFromFile() {
  std::lock_guard<std::mutex> persistenceLock(persistenceMutex);
  if (loaded) return true;

  // Try the current PersistableStore JSON format first.
  if (Storage.exists(getFilePath())) {
    const bool result = PersistableStore<WifiCredentialStore>::loadFromFile();
    loaded = true;
    return result;
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

void WifiCredentialStore::toJson(JsonDocument& doc) const {
  const auto state = snapshot();
  doc["lastConnectedSsid"] = state.lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : state.credentials) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
    obj["password_len"] = static_cast<uint32_t>(cred.password.size());
    obj["password_crc32"] = passwordCrc32(cred.password);
  }
}

bool WifiCredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  Snapshot loaded;
  loaded.lastConnectedSsid = doc["lastConnectedSsid"] | "";
  if (loaded.lastConnectedSsid.size() > MAX_SSID_LENGTH) {
    LOG_ERR("WCS", "Discarding oversized lastConnectedSsid from JSON");
    loaded.lastConnectedSsid.clear();
    needsResave = true;
  }

  const JsonArrayConst arr = doc["credentials"].as<JsonArrayConst>();
  for (JsonObjectConst obj : arr) {
    if (loaded.credentials.size() >= MAX_NETWORKS) break;

    WifiCredential cred;
    cred.ssid = obj["ssid"] | "";
    if (cred.ssid.size() > MAX_SSID_LENGTH) {
      LOG_ERR("WCS", "Discarding Wi-Fi credential with oversized SSID");
      needsResave = true;
      continue;
    }

    const JsonVariantConst encodedVariant = obj["password_obf"];
    const bool hasEncodedPassword = encodedVariant.is<const char*>();
    const char* encoded = encodedVariant | "";
    const JsonVariantConst lengthVariant = obj["password_len"];
    const JsonVariantConst crcVariant = obj["password_crc32"];
    const bool hasLength = !lengthVariant.isNull();
    const bool hasCrc = !crcVariant.isNull();
    const bool hasIntegrityMetadata = hasLength || hasCrc;
    const bool lengthTypeOk = !hasLength || lengthVariant.is<uint32_t>() || lengthVariant.is<int>();
    const bool crcTypeOk = !hasCrc || crcVariant.is<uint32_t>() || crcVariant.is<int>();
    const bool lengthNonNegative = !lengthVariant.is<int>() || lengthVariant.as<int>() >= 0;
    const uint32_t expectedLength = lengthVariant.as<uint32_t>();
    const uint32_t expectedCrc = crcVariant.as<uint32_t>();

    bool decodedOk = false;
    if (hasIntegrityMetadata) {
      if (!hasEncodedPassword || !lengthTypeOk || !crcTypeOk || !lengthNonNegative ||
          expectedLength > MAX_PASSWORD_LENGTH || std::strlen(encoded) > MAX_PASSWORD_B64_LENGTH) {
        LOG_ERR("WCS", "Discarding Wi-Fi password with invalid integrity metadata for '%s'", cred.ssid.c_str());
      } else if (encoded[0] == '\0' && (!hasLength || expectedLength == 0) &&
                 (!hasCrc || expectedCrc == passwordCrc32(""))) {
        decodedOk = true;
      } else {
        bool decodeOk = false;
        bool tooLong = false;
        cred.password = obfuscation::deobfuscateFromBase64(encoded, MAX_PASSWORD_LENGTH, &decodeOk, &tooLong);
        decodedOk = decodeOk && cred.password.size() <= MAX_PASSWORD_LENGTH;
        if (hasLength) decodedOk = decodedOk && cred.password.size() == expectedLength;
        if (hasCrc) decodedOk = decodedOk && passwordCrc32(cred.password) == expectedCrc;
        if (!decodedOk) {
          LOG_ERR("WCS", "Discarding Wi-Fi password with %s for '%s'",
                  tooLong ? "oversized data" : "decode/CRC mismatch", cred.ssid.c_str());
          cred.password.clear();
        }
      }
      if (!hasLength || !hasCrc || !decodedOk) needsResave = true;
    } else if (hasEncodedPassword) {
      // Older JSON used password_obf without integrity metadata.
      if (encoded[0] == '\0') {
        decodedOk = true;
      } else if (std::strlen(encoded) <= MAX_PASSWORD_B64_LENGTH) {
        bool decodeOk = false;
        bool tooLong = false;
        cred.password = obfuscation::deobfuscateFromBase64(encoded, MAX_PASSWORD_LENGTH, &decodeOk, &tooLong);
        decodedOk = decodeOk && cred.password.size() <= MAX_PASSWORD_LENGTH;
        if (!decodedOk) {
          LOG_ERR("WCS", "Discarding Wi-Fi password with %s for '%s'",
                  tooLong ? "oversized data" : "decode failure", cred.ssid.c_str());
          cred.password.clear();
        }
      } else {
        LOG_ERR("WCS", "Discarding oversized Wi-Fi password for '%s'", cred.ssid.c_str());
      }
      needsResave = true;
    } else if (obj["password"].is<const char*>() || obj["password"].is<std::string>()) {
      // Very old JSON stored plaintext. Read once, then rewrite obfuscated.
      cred.password = obj["password"] | "";
      if (cred.password.size() > MAX_PASSWORD_LENGTH) {
        LOG_ERR("WCS", "Discarding oversized legacy Wi-Fi password for '%s'", cred.ssid.c_str());
        cred.password.clear();
      }
      decodedOk = true;
      needsResave = true;
    } else if (!encodedVariant.isNull()) {
      LOG_ERR("WCS", "Discarding Wi-Fi password with invalid encoding for '%s'", cred.ssid.c_str());
      needsResave = true;
    } else {
      // No password field represents an open network.
      decodedOk = true;
      needsResave = true;
    }

    (void)decodedOk;
    loaded.credentials.push_back(std::move(cred));
  }

  replaceState(std::move(loaded));
  if (needsResave) requestResave();
  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", snapshot().credentials.size());
  return true;
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
