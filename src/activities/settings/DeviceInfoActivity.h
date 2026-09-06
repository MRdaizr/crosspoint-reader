#pragma once

#include <I18n.h>

#include <array>
#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"
#include "HalSystem.h"

// Read-only device and firmware diagnostics.  The page deliberately uses the
// same FUI list shell as the other settings pages so button navigation,
// no-icon RoundedRaffExt styling, and touch routing remain consistent.
class DeviceInfoActivity final : public UiListActivity {
 public:
  explicit DeviceInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DeviceInfo", renderer, mappedInput) {}

  void onEnter() override;
  static constexpr int MENU_ITEMS = 9;

 private:
  std::string rowValues[MENU_ITEMS];
  freeink::ui::ListItem rowItems[MENU_ITEMS]{};

  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawFooter() override;
  const char* headerTitle() const override { return tr(STR_ABOUT); }

  void refreshValues();

  HalSystem::HeapInfo heapInfo{};
  HalSystem::DeviceId wifiMac{};
  const char* deviceName = nullptr;
  uint64_t uptimeSeconds = 0;
  uint64_t sdTotalBytes = 0;
  uint64_t sdFreeBytes = 0;
  int chipTemperatureCelsius = 0;
  bool wifiMacAvailable = false;
  bool temperatureAvailable = false;
  bool storageAvailable = false;
};
