#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "activities/UiListActivity.h"

// SD font management page.  The web server remains the transport for large
// .cpfont uploads; this page provides the on-device manifest view, refresh,
// active-family indication and safe deletion workflow.
class FontDownloadActivity final : public UiListActivity {
 public:
  FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 protected:
  int listCount() const override { return static_cast<int>(families_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override { finish(); }

 private:
  void refreshFamilies();
  void confirmDelete(int index);
  void handleDeleteResult(int index, const ActivityResult& result);

  FontInstaller installer_;
  std::vector<std::string> families_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
