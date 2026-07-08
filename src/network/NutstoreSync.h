#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NutstoreConfigStore.h"

enum class NutstoreSyncPhase {
  IDLE,
  ENUMERATING,
  DOWNLOADING,
  DELETING,
  SUCCESS,
  FAILED,
  CANCELLED,
};

struct NutstoreSyncStatus {
  NutstoreSyncPhase phase = NutstoreSyncPhase::IDLE;
  size_t processed = 0;
  size_t total = 0;
  size_t downloaded = 0;
  size_t skipped = 0;
  size_t deleted = 0;
  std::string currentFile;
  std::string message;
};

class NutstoreSync {
 public:
  using StatusCallback = std::function<void(const NutstoreSyncStatus&)>;

  static bool run(const NutstoreConfig& config, NutstoreSyncStatus& status, StatusCallback callback,
                  bool* cancelFlag = nullptr);
  static const char* phaseName(NutstoreSyncPhase phase);

 private:
  static bool isAllowedReadingFile(const std::string& path);
};
