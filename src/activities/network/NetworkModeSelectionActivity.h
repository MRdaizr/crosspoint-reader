#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, NUTSTORE_SYNC, AIRPAGE };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "Nutstore Sync" - Sync files from Nutstore WebDAV
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("NetworkModeSelection", renderer, mappedInput) {}

  static constexpr int MENU_ITEM_COUNT = 5;

  void onEnter() override;
  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override { return MENU_ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override { return tr(STR_FILE_TRANSFER); }

  freeink::ui::ListItem rowItems_[MENU_ITEM_COUNT]{};
};
