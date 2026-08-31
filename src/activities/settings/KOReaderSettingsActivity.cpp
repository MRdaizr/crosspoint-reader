#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
const StrId menuNames[KOReaderSettingsActivity::MENU_ITEMS] = {
    StrId::STR_USERNAME,      StrId::STR_PASSWORD,      StrId::STR_SYNC_SERVER_URL, StrId::STR_DOCUMENT_MATCHING,
    StrId::STR_SEND_METADATA, StrId::STR_SYNC_BEHAVIOR, StrId::STR_SIGN_UP,         StrId::STR_AUTHENTICATE};
}  // namespace

void KOReaderSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < MENU_ITEMS; ++i) {
    rowItems[i].label = I18N.get(menuNames[i]);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    rowItems[i].icon = {};
  }
}

void KOReaderSettingsActivity::handleSelection() {
  if (nav.selected == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(kb.text, KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (nav.selected == 1) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                KOREADER_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
            KOREADER_STORE.saveToFile();
          }
        });
  } else if (nav.selected == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOREADER_STORE.setServerUrl(urlToSave);
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (nav.selected == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (nav.selected == 4) {
    // Send metadata - toggle on/off
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (nav.selected == 5) {
    // Sync behavior - toggle between Ask and Smart
    const auto current = KOREADER_STORE.getSyncBehavior();
    const auto newBehavior = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                                 : KOReaderSyncBehavior::ASK_EVERY_TIME;
    KOREADER_STORE.setSyncBehavior(newBehavior);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (nav.selected == 6) {
    // Sign up - create an account with the configured credentials
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (nav.selected == 7) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  rowValues[0] = KOREADER_STORE.getUsername().empty() ? tr(STR_NOT_SET) : KOREADER_STORE.getUsername();
  rowValues[1] = KOREADER_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
  rowValues[2] = KOREADER_STORE.getServerUrl().empty() ? tr(STR_DEFAULT_VALUE) : KOREADER_STORE.getServerUrl();
  rowValues[3] = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
  rowValues[4] = KOREADER_STORE.getSendMetadata() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  rowValues[5] = KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART ? tr(STR_SMART_SYNC)
                                                                                   : tr(STR_ASK_EVERY_TIME);
  rowValues[6] = KOREADER_STORE.hasCredentials() ? std::string() : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
  rowValues[7] = KOREADER_STORE.hasCredentials() ? std::string() : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
  for (int i = 0; i < MENU_ITEMS; ++i) rowItems[i].value = rowValues[i].empty() ? nullptr : rowValues[i].c_str();
  fui::ListProps props;
  props.items = rowItems;
  props.count = MENU_ITEMS;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void KOReaderSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= MENU_ITEMS) return;
  app.clearTapFlash();
  nav.selected = index;
  handleSelection();
}
