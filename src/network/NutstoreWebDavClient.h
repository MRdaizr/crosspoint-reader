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

  NutstoreWebDavClient(std::string baseUrl, std::string username, std::string password);

  bool listRecursive(const std::string& remotePath, std::vector<NutstoreRemoteEntry>& entries, std::string& error);
  bool downloadFile(const NutstoreRemoteEntry& entry, const std::string& destPath, ProgressCallback progress,
                    std::string& error);

 private:
  std::string baseUrl;
  std::string username;
  std::string password;
  std::string origin;
  std::string basePath;
  std::string rootPath;

  bool propfindDepth1(const std::string& collectionUrl, std::vector<NutstoreRemoteEntry>& entries, std::string& error);
  std::string buildCollectionUrl(const std::string& remotePath) const;
  std::string relativeFromHref(const std::string& href) const;
};
