#pragma once

#include <functional>
#include <string>
#include <vector>

struct NutstoreRemoteEntry {
  std::string href;
  std::string relativePath;
  bool isDirectory = false;
  size_t size = 0;
  std::string lastModified;
};

class NutstoreWebDavClient {
 public:
  using ProgressCallback = std::function<void(size_t done, size_t total)>;
  using EntryCallback = std::function<bool(NutstoreRemoteEntry&& entry, std::string& error)>;

  NutstoreWebDavClient(std::string baseUrl, std::string username, std::string password);

  bool listRecursive(const std::string& remotePath, EntryCallback onEntry, std::string& error);
  bool downloadFile(const NutstoreRemoteEntry& entry, const std::string& destPath, ProgressCallback progress,
                    std::string& error);

 private:
  std::string baseUrl;
  std::string username;
  std::string password;
  std::string origin;
  std::string basePath;
  std::string rootPath;

  std::string buildCollectionUrl(const std::string& remotePath) const;
  std::string relativeFromHref(const std::string& href) const;
};
