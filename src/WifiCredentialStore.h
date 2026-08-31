#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

// Safe, password-free view used by the WebServer and settings UI.
struct WifiCredentialSummary {
  std::string ssid;
  bool hasPassword = false;
  bool isLastConnected = false;
};

/**
 * Singleton class for storing WiFi credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */
class WifiCredentialStore : public PersistableStore<WifiCredentialStore> {
 private:
  struct Snapshot {
    std::vector<WifiCredential> credentials;
    std::string lastConnectedSsid;
  };

  static WifiCredentialStore instance;
  std::vector<WifiCredential> credentials;
  std::string lastConnectedSsid;
  mutable std::mutex stateMutex;
  mutable std::mutex persistenceMutex;
  mutable bool loaded = false;

  static constexpr size_t MAX_NETWORKS = 8;

  // Private constructor for singleton
  WifiCredentialStore() = default;

  bool loadFromBinaryFile(Snapshot& loaded) const;
  Snapshot snapshot() const;
  void replaceState(Snapshot&& loaded);
  bool saveToFileLocked() const;

  static const char* getFilePath() { return "/.crosspoint/wifi.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  friend class PersistableStore<WifiCredentialStore>;

 public:
  // Delete copy constructor and assignment
  WifiCredentialStore(const WifiCredentialStore&) = delete;
  WifiCredentialStore& operator=(const WifiCredentialStore&) = delete;

  // Get singleton instance
  static WifiCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();
  bool ensureLoaded() const;

  // Credential management
  bool addCredential(const std::string& ssid, const std::string& password);
  bool removeCredential(const std::string& ssid);
  std::optional<WifiCredential> findCredential(const std::string& ssid) const;
  std::optional<WifiCredential> getCredentialAt(size_t index) const;
  std::vector<WifiCredentialSummary> getCredentialSummaries() const;

  // Check if a network is saved
  bool hasSavedCredential(const std::string& ssid) const;

  // Last connected network
  void setLastConnectedSsid(const std::string& ssid);
  std::string getLastConnectedSsid() const;
  void clearLastConnectedSsid();

  // Clear all credentials
  void clearAll();
};

// Helper macro to access credentials store
#define WIFI_STORE WifiCredentialStore::getInstance()
