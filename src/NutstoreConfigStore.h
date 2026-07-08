#pragma once

#include <string>

struct NutstoreConfig {
  bool enabled = false;
  std::string baseUrl = "https://dav.jianguoyun.com/dav/";
  std::string username;
  std::string password;
  std::string remotePath = "/";
  std::string localPath = "/Nutstore";
  bool recursive = true;
  bool mirrorDelete = true;
};

class NutstoreConfigStore {
 public:
  static NutstoreConfigStore& getInstance() { return instance; }

  bool loadFromFile();
  bool saveToFile() const;

  const NutstoreConfig& get() const { return config; }
  NutstoreConfig& mutableConfig() { return config; }
  bool hasCredentials() const { return !config.username.empty() && !config.password.empty(); }

 private:
  static NutstoreConfigStore instance;
  NutstoreConfig config;
};

#define NUTSTORE_CONFIG NutstoreConfigStore::getInstance()
