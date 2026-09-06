#include "DeviceInfoActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr uint64_t BYTES_PER_TENTH_GB = 100000000ULL;
constexpr StrId LABELS[DeviceInfoActivity::MENU_ITEMS] = {
    StrId::STR_ABOUT_FIRMWARE_NAME,    StrId::STR_ABOUT_FIRMWARE_VERSION,   StrId::STR_ABOUT_DEVICE_MODEL,
    StrId::STR_ABOUT_WIFI_MAC_ADDRESS, StrId::STR_ABOUT_CHIP_TEMPERATURE,   StrId::STR_ABOUT_UPTIME,
    StrId::STR_ABOUT_HEAP_FREE_TOTAL,  StrId::STR_ABOUT_LARGEST_HEAP_BLOCK, StrId::STR_ABOUT_SD_USED_TOTAL,
};
}  // namespace

void DeviceInfoActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < MENU_ITEMS; ++i) {
    rowItems[i].label = I18N.get(LABELS[i]);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    rowItems[i].icon = {};
  }
  refreshValues();
}

void DeviceInfoActivity::refreshValues() {
  heapInfo = HalSystem::getHeapInfo();
  deviceName = HalSystem::getDeviceModel();
  storageAvailable = Storage.getSpace(sdTotalBytes, sdFreeBytes);
#ifdef SIMULATOR
  wifiMacAvailable = HalSystem::getDeviceId(wifiMac);
  uptimeSeconds = millis() / 1000;
#else
  wifiMacAvailable = HalSystem::getWifiStationMac(wifiMac);
  float temperature = 0.0f;
  temperatureAvailable = HalSystem::getChipTemperatureCelsius(temperature);
  if (temperatureAvailable) chipTemperatureCelsius = static_cast<int>(std::lround(temperature));
  uptimeSeconds = HalSystem::getUptimeSeconds();
#endif

  char value[64];
  rowValues[0] = tr(STR_CROSSPOINT);
  rowValues[1] = CROSSPOINT_VERSION;
  rowValues[2] = deviceName ? deviceName : tr(STR_NOT_AVAILABLE);
  if (wifiMacAvailable) {
    snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", wifiMac[0], wifiMac[1], wifiMac[2], wifiMac[3],
             wifiMac[4], wifiMac[5]);
    rowValues[3] = value;
  } else {
    rowValues[3] = tr(STR_NOT_AVAILABLE);
  }
  if (temperatureAvailable) {
    snprintf(value, sizeof(value), "%d", chipTemperatureCelsius);
    rowValues[4] = value;
  } else {
    rowValues[4] = tr(STR_NOT_AVAILABLE);
  }

  const uint64_t totalMinutes = uptimeSeconds / 60;
  snprintf(value, sizeof(value), "%llu:%02llu:%02llu", static_cast<unsigned long long>(totalMinutes / (24 * 60)),
           static_cast<unsigned long long>(totalMinutes / 60 % 24),
           static_cast<unsigned long long>(totalMinutes % 60));
  rowValues[5] = value;

  snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(heapInfo.freeBytes / 1024),
           static_cast<unsigned long>(heapInfo.totalBytes / 1024));
  rowValues[6] = value;
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(heapInfo.largestFreeBlockBytes / 1024));
  rowValues[7] = value;

  if (!storageAvailable) {
    rowValues[8] = tr(STR_NOT_AVAILABLE);
  } else {
    const uint64_t usedTenths = (sdTotalBytes - sdFreeBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
    const uint64_t totalTenths = (sdTotalBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
    snprintf(value, sizeof(value), "%llu.%llu / %llu.%llu", static_cast<unsigned long long>(usedTenths / 10),
             static_cast<unsigned long long>(usedTenths % 10), static_cast<unsigned long long>(totalTenths / 10),
             static_cast<unsigned long long>(totalTenths % 10));
    rowValues[8] = value;
  }
}

void DeviceInfoActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  for (int i = 0; i < MENU_ITEMS; ++i) {
    rowItems[i].value = rowValues[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = MENU_ITEMS;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  props.valueText = screen.theme().smallText;
  syncListViewport(screen, props);
  screen.list(props);
}

void DeviceInfoActivity::activateIndex(const int index) {
  // All rows are diagnostics only.  Keep the action registered so touch and
  // Confirm remain harmless, while Back continues to be handled by the base.
  if (index < 0 || index >= MENU_ITEMS) return;
  app.clearTapFlash();
}

void DeviceInfoActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
