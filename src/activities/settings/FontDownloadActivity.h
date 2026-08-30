#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "activities/UiListActivity.h"

// SD font management page.  Installed families can be deleted locally; the
// optional Wi-Fi browser streams the versioned manifest and downloads checked
// .cpfont files without buffering them in RAM.  The web upload path remains
// available for custom packs.
class FontDownloadActivity final : public UiListActivity {
 public:
  FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  bool preventAutoSleep() override;
  bool skipLoopDelay() override { return true; }

 protected:
  void onEnter() override;
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;
  bool handleCustomInput() override;

 private:
  enum class State : uint8_t { Installed, Loading, Remote, Downloading, Complete, Error };

  struct RemoteFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct RemoteFamily {
    std::string name;
    std::string description;
    std::vector<RemoteFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  void refreshFamilies();
  void openRemoteBrowser();
  void onWifiSelectionComplete(bool success);
  bool fetchManifest();
  void rebuildRemoteRows();
  void downloadFamily(int index);
  bool computeFileCrc32(const char* path, uint32_t& outCrc) const;
  static std::string formatSize(size_t bytes);
  void confirmDelete(int index);
  void handleDeleteResult(int index, const ActivityResult& result);

  State state_ = State::Installed;
  FontInstaller installer_;
  std::vector<std::string> families_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<RemoteFamily> remoteFamilies_;
  std::string remoteBaseUrl_;
  std::string errorMessage_;
  int downloadingFamilyIndex_ = -1;
  size_t downloadingFileIndex_ = 0;
  size_t downloadingFileCount_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  bool cancelRequested_ = false;
};
