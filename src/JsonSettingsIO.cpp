#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"

namespace {
constexpr size_t WIFI_MAX_PASSWORD_LENGTH = 64;
// Enum settings are persisted by ordinal. Bump this whenever an ordinal
// layout changes and migrate older files before the generic loader runs.
constexpr uint8_t SETTINGS_SCHEMA_VERSION = 3;
constexpr size_t WIFI_MAX_SSID_LENGTH = 32;
// Base64 expands at most 64 plaintext bytes to 88 characters. Reject larger
// strings before handing them to the decoder, which otherwise allocates based
// on attacker-controlled JSON input.
constexpr size_t WIFI_MAX_PASSWORD_B64_LENGTH = ((WIFI_MAX_PASSWORD_LENGTH + 2) / 3) * 4;

uint32_t wifiPasswordCrc32(const std::string& password) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : password) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return ~crc;
}
}  // namespace

// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  JsonArray recentOverlayArr = doc["recentOverlaySleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentOverlayArr.add(s.recentOverlaySleepImages[i]);
  doc["recentOverlaySleepPos"] = s.recentOverlaySleepPos;
  doc["recentOverlaySleepFill"] = s.recentOverlaySleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["showBootScreen"] = s.showBootScreen;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  memset(s.recentOverlaySleepImages, 0, sizeof(s.recentOverlaySleepImages));
  JsonArrayConst recentOverlayArr = doc["recentOverlaySleepImages"];
  const int actualOverlayCount = recentOverlayArr.isNull()
                                     ? 0
                                     : std::min(static_cast<int>(recentOverlayArr.size()),
                                                static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualOverlayCount; i++) {
    s.recentOverlaySleepImages[i] = recentOverlayArr[i] | static_cast<uint16_t>(0);
  }
  s.recentOverlaySleepPos = doc["recentOverlaySleepPos"] | static_cast<uint8_t>(0);
  if (s.recentOverlaySleepPos >= CrossPointState::SLEEP_RECENT_COUNT) {
    s.recentOverlaySleepPos = actualOverlayCount > 0 ? s.recentOverlaySleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  }
  s.recentOverlaySleepFill = doc["recentOverlaySleepFill"] | static_cast<uint8_t>(0);
  s.recentOverlaySleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentOverlaySleepFill), actualOverlayCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.showBootScreen = doc["showBootScreen"] | true;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;
  doc["settingsSchema"] = SETTINGS_SCHEMA_VERSION;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  doc["fontFamily"] = CrossPointSettings::normalizeBuiltinFontFamily(s.fontFamily);
  // Reader size is an actual point size since the text-settings migration.
  // Keep the old key out of newly written files; the loader below accepts it
  // for one-way migration from pre-1.5 settings.
  const bool hasSdFont = s.sdFontFamilyName[0] != '\0';
#ifdef OMIT_FONTS
  doc["fontPointSize"] = hasSdFont ? s.fontPointSize : CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
#else
  doc["fontPointSize"] = s.fontPointSize;
#endif
  // SD card font family name — not in SettingsList, save manually
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // SD dictionary folder is selected through the runtime enum in SettingsList
  // and therefore has no member pointer for the generic serializer.
  if (s.dictionaryName[0] != '\0') {
    doc["dictionaryName"] = s.dictionaryName;
  }

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Version 2 was written by the interim standard X3/X4 profile, where
  // BLANK=4 and COVER_CUSTOM=5. Version 3 restores the upstream ordinals,
  // including TRANSPARENT_CUSTOM=7 and REFRESH_NEVER=5. Unversioned files
  // are treated as upstream-layout files for compatibility with the base
  // firmware's JSON settings.
  const uint8_t settingsSchema = doc["settingsSchema"] | static_cast<uint8_t>(1);
  if (settingsSchema == 2) {
    if (needsResave) *needsResave = true;

    // Version 2: BLANK=4, COVER_CUSTOM=5. Version 3: COVER_CUSTOM=4,
    // BLANK=5. The former transparent value was not exposed in version 2.
    if (!doc["sleepScreen"].isNull()) {
      const uint8_t value = doc["sleepScreen"] | static_cast<uint8_t>(0);
      if (value == 4) {
        doc["sleepScreen"] = CrossPointSettings::BLANK;
      } else if (value == 5) {
        doc["sleepScreen"] = CrossPointSettings::COVER_CUSTOM;
      }
    }
  }

  // PWR_CONFIRM is X4 Pro-only in the upstream firmware. Ignore is safe on
  // standard X3/X4 hardware and avoids interpreting it as another action.
  if (settingsSchema < SETTINGS_SCHEMA_VERSION && !doc["shortPwrBtn"].isNull() &&
      (doc["shortPwrBtn"] | static_cast<uint8_t>(0)) == 5) {
    doc["shortPwrBtn"] = CrossPointSettings::IGNORE;
    if (needsResave) *needsResave = true;
  }

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", &ok);
        if (!ok || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)CrossPointSettings::SLEEP_10_MIN, CrossPointSettings::SLEEP_TIMEOUT_COUNT,
              (uint8_t)CrossPointSettings::SLEEP_10_MIN);
    s.sleepTimeoutMinutes = CrossPointSettings::sleepTimeoutEnumToMinutes(legacyValue);
    if (needsResave) *needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);

  // Reader font size migration.  New files use fontPointSize directly.  A
  // legacy fontSize value is unambiguous because old files stored only the
  // four enum slots 0..3.
  if (!doc["fontPointSize"].isNull()) {
    const uint8_t stored = doc["fontPointSize"] | CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
    s.fontPointSize = stored >= 1 ? stored : CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
  } else if (!doc["fontSize"].isNull()) {
    const uint8_t legacy = doc["fontSize"] | static_cast<uint8_t>(1);
    if (legacy <= CrossPointSettings::LEGACY_FONT_SIZE_MAX) {
      s.fontPointSize = static_cast<uint8_t>(12 + legacy * 2);
      if (needsResave) *needsResave = true;
    } else {
      s.fontPointSize = CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
      if (needsResave) *needsResave = true;
    }
  }

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  s.fontFamily = clamp(storedFontFamily, CrossPointSettings::BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  const char* dictionaryName = doc["dictionaryName"] | "";
  strncpy(s.dictionaryName, dictionaryName, sizeof(s.dictionaryName) - 1);
  s.dictionaryName[sizeof(s.dictionaryName) - 1] = '\0';
  if (storedFontFamily == CrossPointSettings::LEGACY_OPENDYSLEXIC && s.sdFontFamilyName[0] == '\0') {
    s.fontFamily = CrossPointSettings::NOTOSERIF;
    strncpy(s.sdFontFamilyName, "OpenDyslexic", sizeof(s.sdFontFamilyName) - 1);
    s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
    if (needsResave) *needsResave = true;
  } else if (storedFontFamily >= CrossPointSettings::BUILTIN_FONT_COUNT) {
    if (needsResave) *needsResave = true;
  }

#ifdef OMIT_FONTS
  // Migrate settings created by a full build to the slim reader profile.  SD
  // font selections remain intact; only their built-in fallback is normalized.
  if (s.fontFamily != CrossPointSettings::NOTOSERIF) {
    s.fontFamily = CrossPointSettings::NOTOSERIF;
    if (needsResave) *needsResave = true;
  }
  if (s.sdFontFamilyName[0] == '\0' && s.fontPointSize != CrossPointSettings::DEFAULT_FONT_POINT_SIZE) {
    s.fontPointSize = CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
    if (needsResave) *needsResave = true;
  }
#endif

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  const auto state = store.snapshot();
  JsonDocument doc;
  doc["lastConnectedSsid"] = state.lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : state.credentials) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
    obj["password_len"] = static_cast<uint32_t>(cred.password.size());
    obj["password_crc32"] = wifiPasswordCrc32(cred.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WCS", "JSON parse error: %s", error.c_str());
    return false;
  }

  WifiCredentialStore::Snapshot loaded;
  loaded.lastConnectedSsid = doc["lastConnectedSsid"] | std::string("");
  if (loaded.lastConnectedSsid.size() > WIFI_MAX_SSID_LENGTH) {
    LOG_ERR("WCS", "Discarding oversized lastConnectedSsid from JSON");
    loaded.lastConnectedSsid.clear();
    if (needsResave) *needsResave = true;
  }

  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (loaded.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    if (cred.ssid.size() > WIFI_MAX_SSID_LENGTH) {
      LOG_ERR("WCS", "Discarding Wi-Fi credential with oversized SSID");
      if (needsResave) *needsResave = true;
      continue;
    }

    const auto encodedVariant = obj["password_obf"];
    const bool hasEncodedPassword = encodedVariant.is<const char*>();
    const char* encoded = encodedVariant | "";
    const auto lengthVariant = obj["password_len"];
    const auto crcVariant = obj["password_crc32"];
    const bool hasLength = !lengthVariant.isNull();
    const bool hasCrc = !crcVariant.isNull();
    const bool hasIntegrityMetadata = hasLength || hasCrc;
    const bool lengthTypeOk = !hasLength || lengthVariant.is<uint32_t>() || lengthVariant.is<int>();
    const bool crcTypeOk = !hasCrc || crcVariant.is<uint32_t>() || crcVariant.is<int>();
    const bool lengthNonNegative = !lengthVariant.is<int>() || lengthVariant.as<int>() >= 0;
    const uint32_t expectedLength = lengthVariant.as<uint32_t>();
    const uint32_t expectedCrc = crcVariant.as<uint32_t>();

    bool decodedOk = false;
    if (hasIntegrityMetadata) {
      // Both fields are optional for backwards compatibility. If only one is
      // present, validate the available field and rewrite the complete pair.
      if (!hasEncodedPassword || !lengthTypeOk || !crcTypeOk || !lengthNonNegative ||
          expectedLength > WIFI_MAX_PASSWORD_LENGTH || std::strlen(encoded) > WIFI_MAX_PASSWORD_B64_LENGTH) {
        LOG_ERR("WCS", "Discarding Wi-Fi password with invalid integrity metadata for '%s'", cred.ssid.c_str());
      } else if (encoded[0] == '\0' && (!hasLength || expectedLength == 0) &&
                 (!hasCrc || expectedCrc == wifiPasswordCrc32(""))) {
        decodedOk = true;
      } else {
        bool decodeOk = false;
        bool tooLong = false;
        cred.password = obfuscation::deobfuscateFromBase64(encoded, WIFI_MAX_PASSWORD_LENGTH, &decodeOk, &tooLong);
        decodedOk = decodeOk && cred.password.size() <= WIFI_MAX_PASSWORD_LENGTH;
        if (hasLength) decodedOk = decodedOk && cred.password.size() == expectedLength;
        if (hasCrc) decodedOk = decodedOk && wifiPasswordCrc32(cred.password) == expectedCrc;
        if (!decodedOk) {
          LOG_ERR("WCS", "Discarding Wi-Fi password with %s for '%s'", tooLong ? "oversized data" : "decode/CRC mismatch",
                  cred.ssid.c_str());
          cred.password.clear();
        }
      }
      if (needsResave && (!hasLength || !hasCrc || !decodedOk)) *needsResave = true;
    } else if (hasEncodedPassword) {
      // Old JSON had password_obf but no integrity metadata. Keep it readable
      // and rewrite it into the checked format below.
      if (encoded[0] == '\0') {
        decodedOk = true;
      } else if (std::strlen(encoded) <= WIFI_MAX_PASSWORD_B64_LENGTH) {
        bool decodeOk = false;
        bool tooLong = false;
        cred.password = obfuscation::deobfuscateFromBase64(encoded, WIFI_MAX_PASSWORD_LENGTH, &decodeOk, &tooLong);
        decodedOk = decodeOk && cred.password.size() <= WIFI_MAX_PASSWORD_LENGTH;
        if (!decodedOk) {
          LOG_ERR("WCS", "Discarding Wi-Fi password with %s for '%s'", tooLong ? "oversized data" : "decode failure",
                  cred.ssid.c_str());
          cred.password.clear();
        }
      } else {
        LOG_ERR("WCS", "Discarding oversized Wi-Fi password for '%s'", cred.ssid.c_str());
      }
      if (needsResave) *needsResave = true;
    } else if (obj["password"].is<const char*>() || obj["password"].is<std::string>()) {
      // Very old JSON stored plaintext. Read it once, then rewrite obfuscated.
      cred.password = obj["password"] | std::string("");
      if (cred.password.size() > WIFI_MAX_PASSWORD_LENGTH) {
        LOG_ERR("WCS", "Discarding oversized legacy Wi-Fi password for '%s'", cred.ssid.c_str());
        cred.password.clear();
      }
      decodedOk = true;
      if (needsResave) *needsResave = true;
    } else if (!encodedVariant.isNull()) {
      LOG_ERR("WCS", "Discarding Wi-Fi password with invalid encoding for '%s'", cred.ssid.c_str());
      if (needsResave) *needsResave = true;
    } else {
      // No password field represents an open network.
      decodedOk = true;
      if (needsResave) *needsResave = true;
    }

    (void)decodedOk;
    loaded.credentials.push_back(std::move(cred));
  }

  store.replaceState(std::move(loaded));
  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.snapshot().credentials.size());
  return true;
}

// ---- RecentBooksStore ----

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.recentBooks.clear();
  JsonArray arr = doc["books"].as<JsonArray>();
  store.recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(10)));
  for (JsonObject obj : arr) {
    if (store.getCount() >= 10) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    store.recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}

// ---- OpdsServerStore ----
// Follows the same save/load pattern as WifiCredentialStore above.
// Passwords are XOR-obfuscated with the device MAC and base64-encoded ("password_obf" key).

bool JsonSettingsIO::saveOpds(const OpdsServerStore& store, const char* path) {
  JsonDocument doc;

  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : store.getServers()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadOpds(OpdsServerStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("OPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.servers.clear();
  JsonArray arr = doc["servers"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    // Try the obfuscated key first; fall back to plaintext "password" for
    // files written before obfuscation was added (or hand-edited JSON).
    bool ok = false;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (!server.password.empty() && needsResave) *needsResave = true;
    }
    store.servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", store.servers.size());
  return true;
}

// ---- Bookmarks ----

bool JsonSettingsIO::saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) {
      obj["vo"] = bookmark.visibleTextOffset;
    }
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("BKM", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.clear();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | std::string("");
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | std::string("");
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.hasVisibleTextOffset = true;
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
    }
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}
