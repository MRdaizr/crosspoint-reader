#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "SettingsList.h"
#include "fontIds.h"

void readAndValidate(HalFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
// Enum settings are persisted by ordinal. Keep the version stable while
// accepting the interim layout during migration.
constexpr uint8_t SETTINGS_SCHEMA_VERSION = 3;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

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

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  doc["settingsSchema"] = SETTINGS_SCHEMA_VERSION;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries are stored separately and are not part of settings.json.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = reinterpret_cast<const char*>(this) + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = this->*(info.valuePtr);
    }
  }

  // Front button remap is edited by its own activity and is not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  doc["fontFamily"] = normalizeBuiltinFontFamily(fontFamily);
#ifdef OMIT_FONTS
  const bool hasSdFont = sdFontFamilyName[0] != '\0';
  doc["fontSize"] = hasSdFont ? fontPointSize : DEFAULT_FONT_POINT_SIZE;
#else
  doc["fontSize"] = fontPointSize;
#endif
  if (sdFontFamilyName[0] != '\0') doc["sdFontFamilyName"] = sdFontFamilyName;
  if (dictionaryName[0] != '\0') doc["dictionaryName"] = dictionaryName;

  // Language is stored as an ISO code so enum reorderings do not change it.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  auto clamp = [](const uint8_t value, const uint8_t maxValue, const uint8_t def) -> uint8_t {
    return value < maxValue ? value : def;
  };
  bool needsResave = false;

  // Version 2 temporarily swapped BLANK/COVER_CUSTOM. Version 3 restores the
  // stable upstream ordinals; unversioned files use the upstream layout.
  const uint8_t settingsSchema = doc["settingsSchema"] | static_cast<uint8_t>(1);
  if (settingsSchema == 2) {
    needsResave = true;
  }

  // PWR_CONFIRM belongs to another hardware profile; do not interpret it as
  // a valid action on this X3/X4 build.
  if (settingsSchema < SETTINGS_SCHEMA_VERSION && !doc["shortPwrBtn"].isNull() &&
      (doc["shortPwrBtn"] | static_cast<uint8_t>(0)) == 5) {
    needsResave = true;
  }

  if (doc["statusBarChapterPageCount"].isNull()) applyLegacyStatusBarSettings(*this);

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      char* destPtr = reinterpret_cast<char*>(this) + info.stringOffset;
      const std::string fieldDefault = destPtr;
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", &ok);
        if (!ok || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault) needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        continue;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = this->*(info.valuePtr);
      uint8_t value = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        value = clamp(value, static_cast<uint8_t>(info.enumValues.size()), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        value = clamp(value, static_cast<uint8_t>(2), fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (value < info.valueRange.min) value = info.valueRange.min;
        if (value > info.valueRange.max) value = info.valueRange.max;
      }
      this->*(info.valuePtr) = value;
    }
  }

  // Apply schema-2 ordinal fixes after the generic loop because fromJson takes
  // a const view of the parsed document.
  if (settingsSchema == 2 && !doc["sleepScreen"].isNull()) {
    const uint8_t value = doc["sleepScreen"] | static_cast<uint8_t>(0);
    if (value == 4) {
      sleepScreen = BLANK;
    } else if (value == 5) {
      sleepScreen = COVER_CUSTOM;
    }
  }
  if (settingsSchema < SETTINGS_SCHEMA_VERSION && !doc["shortPwrBtn"].isNull() &&
      (doc["shortPwrBtn"] | static_cast<uint8_t>(0)) == 5) {
    shortPwrBtn = IGNORE;
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | static_cast<uint8_t>(SLEEP_10_MIN), SLEEP_TIMEOUT_COUNT, SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }

  frontButtonBack = clamp(doc["frontButtonBack"] | static_cast<uint8_t>(FRONT_HW_BACK),
                           FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm = clamp(doc["frontButtonConfirm"] | static_cast<uint8_t>(FRONT_HW_CONFIRM),
                             FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | static_cast<uint8_t>(FRONT_HW_LEFT),
                          FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight = clamp(doc["frontButtonRight"] | static_cast<uint8_t>(FRONT_HW_RIGHT),
                           FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateFrontButtonMapping(*this);

  if (!doc["fontPointSize"].isNull()) {
    // Compatibility with the interim local format used before the upstream
    // dynamic font-size setting was adopted.
    const uint8_t stored = doc["fontPointSize"] | DEFAULT_FONT_POINT_SIZE;
    fontPointSize = stored >= 1 ? stored : DEFAULT_FONT_POINT_SIZE;
  } else if (!doc["fontSize"].isNull()) {
    // Upstream stores the physical point size under fontSize. Older files
    // stored a 0..3 Small/Medium/Large/Extra-large slot under the same key;
    // those values are unambiguous because no renderable font is 0..3 pt.
    const uint8_t stored = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
    if (stored <= LEGACY_FONT_SIZE_MAX) {
      fontPointSize = static_cast<uint8_t>(12 + stored * 2);
      needsResave = true;
    } else {
      fontPointSize = stored;
    }
  }

  const uint8_t storedFontFamily = doc["fontFamily"] | static_cast<uint8_t>(0);
  fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, 0);
  const char* sdFont = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sdFont, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  const char* dictionary = doc["dictionaryName"] | "";
  strncpy(dictionaryName, dictionary, sizeof(dictionaryName) - 1);
  dictionaryName[sizeof(dictionaryName) - 1] = '\0';

  if (storedFontFamily == LEGACY_OPENDYSLEXIC && sdFontFamilyName[0] == '\0') {
    fontFamily = NOTOSERIF;
    strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
    sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
    needsResave = true;
  } else if (storedFontFamily >= BUILTIN_FONT_COUNT) {
    needsResave = true;
  }

#ifdef OMIT_FONTS
  if (fontFamily != NOTOSERIF) {
    fontFamily = NOTOSERIF;
    needsResave = true;
  }
  if (sdFontFamilyName[0] == '\0' && fontPointSize != DEFAULT_FONT_POINT_SIZE) {
    fontPointSize = DEFAULT_FONT_POINT_SIZE;
    needsResave = true;
  }
#endif

  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  if (needsResave) requestResave();
  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

bool CrossPointSettings::loadFromFile() {
  // Try JSON first
  if (Storage.exists(getFilePath())) {
    const bool result = PersistableStore<CrossPointSettings>::loadFromFile();
    migrateLanguageBinaryFile();
    return result;
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      migrateLanguageBinaryFile();
      if (PersistableStore<CrossPointSettings>::saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        return true;
      }
      LOG_ERR("CPS", "Failed to save migrated settings to JSON");
      return false;
    }
  }

  // No settings files at all -- check for standalone language.bin.
  return migrateLanguageBinaryFile();
}

bool CrossPointSettings::migrateLanguageBinaryFile() {
  // V1_LANGUAGES / V1_LANGUAGE_COUNT are emitted by gen_i18n.py with the
  // frozen enum order from 2f969a9.
  if (!Storage.exists(LANG_FILE_BIN)) return false;

  HalFile f;
  if (Storage.openFileForRead("CPS", LANG_FILE_BIN, f)) {
    uint8_t version;
    serialization::readPod(f, version);
    if (version == 1) {
      uint8_t oldIndex;
      serialization::readPod(f, oldIndex);
      if (oldIndex < V1_LANGUAGE_COUNT) {
        language = static_cast<uint8_t>(V1_LANGUAGES[oldIndex]);
      }
    }
  }
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into settings.json");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      uint8_t legacyFontFamily;
      serialization::readPod(inputFile, legacyFontFamily);
      if (legacyFontFamily < BUILTIN_FONT_COUNT) {
        fontFamily = legacyFontFamily;
      } else if (legacyFontFamily == LEGACY_OPENDYSLEXIC) {
        fontFamily = NOTOSERIF;
        strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
        sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
      }
    }
    if (++settingsRead >= fileSettingsCount) break;
    // Binary settings v1 stored the abstract 0..3 font-size slot.  Keep the
    // file layout readable and translate it immediately to a point size.
    uint8_t legacyFontSize = 1;
    readAndValidate(inputFile, legacyFontSize, LEGACY_FONT_SIZE_MAX + 1);
    fontPointSize = static_cast<uint8_t>(12 + legacyFontSize * 2);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacySleepTimeout = SLEEP_10_MIN;
    readAndValidate(inputFile, legacySleepTimeout, SLEEP_TIMEOUT_COUNT);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacySleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, frontButtonFollowOrientation);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

#ifdef OMIT_FONTS
  // The binary format predates the slim profile.  Preserve the file layout,
  // but never carry an omitted built-in font into the runtime.
  fontFamily = NOTOSERIF;
  if (sdFontFamilyName[0] == '\0') fontPointSize = DEFAULT_FONT_POINT_SIZE;
#endif

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  // SD card fonts use same compression as Bookerly (the most neutral values)
  if (sdFontFamilyName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
      case EXTRA_WIDE:
        return 1.2f;
    }
  }

  const bool sans = fontFamily == NOTOSANS && isBuiltinFontFamilyAvailable(fontFamily);
  switch (sans ? NOTOSANS : NOTOSERIF) {
    case NOTOSERIF:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
        case EXTRA_WIDE:
          return 1.2f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
        case EXTRA_WIDE:
          return 1.05f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_NEVER:
      return std::numeric_limits<int>::max();
  }
}

uint64_t CrossPointSettings::getDailyGoalMs() const {
  switch (dailyGoalTarget) {
    case DAILY_GOAL_15_MIN:
      return 15ULL * 60ULL * 1000ULL;
    case DAILY_GOAL_45_MIN:
      return 45ULL * 60ULL * 1000ULL;
    case DAILY_GOAL_60_MIN:
      return 60ULL * 60ULL * 1000ULL;
    case DAILY_GOAL_30_MIN:
    default:
      return 30ULL * 60ULL * 1000ULL;
  }
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                       const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  return spec;
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

#ifdef OMIT_FONTS
  // A stale NotoSans/12/16/18 value can exist in an older settings file, but
  // those font arrays are not linked in a slim build.  A valid SD font still
  // wins above; otherwise use the one retained built-in reader font.
  return NOTOSERIF_14_FONT_ID;
#else
  const uint8_t pt = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES,
                                            std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  const bool sans = fontFamily == NOTOSANS;
  switch (pt) {
    case 12:
      return sans ? NOTOSANS_12_FONT_ID : NOTOSERIF_12_FONT_ID;
    case 16:
      return sans ? NOTOSANS_16_FONT_ID : NOTOSERIF_16_FONT_ID;
    case 18:
      return sans ? NOTOSANS_18_FONT_ID : NOTOSERIF_18_FONT_ID;
    case 14:
    default:
      return sans ? NOTOSANS_14_FONT_ID : NOTOSERIF_14_FONT_ID;
  }
#endif
}
