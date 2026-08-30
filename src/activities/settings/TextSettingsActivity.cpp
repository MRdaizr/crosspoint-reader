#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "activities/RenderLock.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;
namespace {
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_FONT, StrId::STR_SIZE, StrId::STR_LAYOUT, StrId::STR_STYLE};
constexpr StrId LAYOUT_ROW_NAME_IDS[] = {StrId::STR_LINE_SPACING, StrId::STR_EXTRA_SPACING, StrId::STR_PARA_ALIGNMENT,
                                         StrId::STR_SCREEN_MARGIN};
constexpr StrId STYLE_ROW_NAME_IDS[] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION, StrId::STR_EMBEDDED_STYLE,
                                        StrId::STR_TEXT_AA};
constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFamilyName, uint8_t fontFamily) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); ++i) {
      if (families[i].name == sdFamilyName) return CrossPointSettings::BUILTIN_FONT_COUNT + i;
    }
  }
  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab)
    : UiTabListActivity("TextSettings", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

const char* TextSettingsActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void TextSettingsActivity::onEnter() {
  UiTabListActivity::onEnter();
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS)});
  if (registry_) {
    for (const auto& family : registry_->getFamilies()) {
      fonts_.push_back({family.name, false,
                        static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + (&family - registry_->getFamilies().data()))});
    }
  }
  currentFamilyIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  rebuildSizeList();
  for (auto& nav : tabNavs) nav.selected = 1;
  tabNavs[static_cast<int>(Tab::Family)].selected = currentFamilyIndex_ + 1;
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
  tabNavs[static_cast<int>(tab_)].selected = 0;
  rebuildRowItems();
}

void TextSettingsActivity::rebuildRowItems() {
  rowValues_.assign(listCount(), {});
  rowItems_.clear();
  rowItems_.reserve(listCount());
  for (int i = 0; i < listCount(); ++i) {
    fui::ListItem item;
    if (tab_ == Tab::Family) item.label = fonts_[i].name.c_str();
    else if (tab_ == Tab::Size) item.label = sizes_[i].name.c_str();
    else if (tab_ == Tab::Layout) item.label = I18N.get(LAYOUT_ROW_NAME_IDS[i]);
    else if (tab_ == Tab::Style) item.label = I18N.get(STYLE_ROW_NAME_IDS[i]);
    item.actionValue = static_cast<int16_t>(i);
    item.icon = {};
    rowItems_.push_back(item);
  }
}

void TextSettingsActivity::rebuildSizeList() {
  const auto points = readerFontPointSizes(registry_, SETTINGS.sdFontFamilyName);
  const uint8_t selected = snapToNearestPointSize(points, SETTINGS.fontPointSize);
  sizes_.clear();
  currentSizeIndex_ = 0;
  for (const uint8_t point : points) {
    char label[12];
    snprintf(label, sizeof(label), "%u pt", point);
    if (point == selected) currentSizeIndex_ = static_cast<int>(sizes_.size());
    sizes_.push_back({label, point});
  }
}

void TextSettingsActivity::onTabAction(const int index) {
  if (optionPopup_.isActive() || index < 0 || index >= static_cast<int>(Tab::Count)) return;
  tab_ = static_cast<Tab>(index);
  rebuildRowItems();
  activeNav().selected = 0;
  activeNav().followOnBuild = true;
  app.clearTapFlash();
  requestUpdate();
}

void TextSettingsActivity::activateIndex(const int index) {
  if (optionPopup_.isActive()) return;
  app.clearTapFlash();
  activateRow(index);
}

bool TextSettingsActivity::handleCustomInput() {
  return optionPopup_.handleInput(renderer, mappedInput, [this] { requestUpdate(); });
}

bool TextSettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) switchTab();
    else activateRow(ringPos() - 1);
    return true;
  }
  return false;
}

void TextSettingsActivity::buildScreen(UiScreen& screen) {
  const int tabTop = afterHeader + previewHeight;
  const int captionHeight = renderer.getTextHeight(UI_10_FONT_ID) + metrics_.verticalSpacing;
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(tabTop), 0,
                                      static_cast<int16_t>(bottomReserved + captionHeight), 0});
  buildTabBar(screen);
  for (int i = 0; i < listCount(); ++i) {
    if (tab_ == Tab::Family) rowValues_[i] = i == currentFamilyIndex_ ? tr(STR_SELECTED) : "";
    else if (tab_ == Tab::Size) rowValues_[i] = i == currentSizeIndex_ ? tr(STR_SELECTED) : "";
    else if (tab_ == Tab::Layout) rowValues_[i] = layoutValueText(i);
    else rowValues_[i] = styleValueText(i);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

const char* TextSettingsActivity::confirmLabelText() const {
  if (ringPos() == 0) return I18N.get(TAB_NAME_IDS[(static_cast<int>(tab_) + 1) % static_cast<int>(Tab::Count)]);
  if (tab_ == Tab::Layout && ringPos() - 1 == static_cast<int>(LayoutRow::ParaSpacing)) return tr(STR_TOGGLE);
  return tab_ == Tab::Style ? tr(STR_TOGGLE) : tr(STR_SELECT);
}

void TextSettingsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, renderer.getScreenWidth(), metrics_.headerHeight},
                 tr(STR_TEXT_SETTINGS));
  const char* family = currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size())
                           ? fonts_[currentFamilyIndex_].name.c_str()
                           : "";
  const char* size = currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size())
                         ? sizes_[currentSizeIndex_].name.c_str()
                         : "";
  textsettings::renderPreview(renderer, previewLayout_, metrics_.previewPadding, metrics_.verticalSpacing, afterHeader,
                              previewHeight, family, size);
  renderUi();
  if (focusedRowHasNoPreview()) {
    const int captionHeight = renderer.getTextHeight(UI_10_FONT_ID) + metrics_.verticalSpacing;
    renderer.drawText(UI_10_FONT_ID, metrics_.previewPadding,
                      afterHeader + usableHeight - captionHeight + metrics_.verticalSpacing, tr(STR_NOT_IN_PREVIEW));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabelText(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void TextSettingsActivity::applyFamily(const int index) {
  if (index < 0 || index >= static_cast<int>(fonts_.size())) return;
  RenderLock lock;
  const auto& font = fonts_[index];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIndex = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIndex < 0 || sdIndex >= static_cast<int>(families.size())) return;
    strncpy(SETTINGS.sdFontFamilyName, families[sdIndex].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }
  sdFontSystem.ensureLoaded(renderer);
  currentFamilyIndex_ = index;
  rebuildSizeList();
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
}

void TextSettingsActivity::applySize(const int index) {
  if (index < 0 || index >= static_cast<int>(sizes_.size())) return;
  RenderLock lock;
  SETTINGS.fontPointSize = sizes_[index].pointSize;
  currentSizeIndex_ = index;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::activateRow(const int row) {
  if (row < 0 || row >= listCount()) return;
  if (tab_ == Tab::Family) {
    if (row != currentFamilyIndex_) applyFamily(row);
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (tab_ == Tab::Size) {
    applySize(row);
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (tab_ == Tab::Layout) {
    confirmLayoutRow(row);
  } else {
    confirmStyleRow(row);
  }
}

void TextSettingsActivity::confirmLayoutRow(const int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int value) { SETTINGS.lineSpacing = static_cast<uint8_t>(value); SETTINGS.saveToFile(); });
      requestUpdate();
      break;
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_PARA_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment, [](int value) { SETTINGS.paragraphAlignment = static_cast<uint8_t>(value); SETTINGS.saveToFile(); });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      for (int margin = CrossPointSettings::SCREEN_MARGIN_MIN; margin <= CrossPointSettings::SCREEN_MARGIN_MAX;
           margin += CrossPointSettings::SCREEN_MARGIN_STEP) options.push_back(std::to_string(margin));
      const int current = (std::clamp<int>(SETTINGS.screenMargin, CrossPointSettings::SCREEN_MARGIN_MIN,
                                           CrossPointSettings::SCREEN_MARGIN_MAX) - CrossPointSettings::SCREEN_MARGIN_MIN) /
                          CrossPointSettings::SCREEN_MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, current, [](int value) {
        SETTINGS.screenMargin = static_cast<uint8_t>(CrossPointSettings::SCREEN_MARGIN_MIN + value * CrossPointSettings::SCREEN_MARGIN_STEP);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    default:
      break;
  }
}

std::string TextSettingsActivity::layoutValueText(const int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: return SETTINGS.lineSpacing < std::size(LINE_SPACING_IDS) ? I18N.get(LINE_SPACING_IDS[SETTINGS.lineSpacing]) : I18N.get(StrId::STR_NORMAL);
    case LayoutRow::ParaSpacing: return SETTINGS.extraParagraphSpacing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case LayoutRow::Alignment: return SETTINGS.paragraphAlignment < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[SETTINGS.paragraphAlignment]) : I18N.get(StrId::STR_JUSTIFY);
    case LayoutRow::ScreenMargin: return std::to_string(SETTINGS.screenMargin);
    default: return {};
  }
}

void TextSettingsActivity::confirmStyleRow(const int row) {
  uint8_t* value = nullptr;
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading: value = &SETTINGS.focusReadingEnabled; break;
    case StyleRow::Hyphenation: value = &SETTINGS.hyphenationEnabled; break;
    case StyleRow::EmbeddedStyle: value = &SETTINGS.embeddedStyle; break;
    case StyleRow::AntiAliasing: value = &SETTINGS.textAntiAliasing; break;
    default: return;
  }
  *value = !*value;
  SETTINGS.saveToFile();
  requestUpdate();
}

std::string TextSettingsActivity::styleValueText(const int row) const {
  bool value = false;
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading: value = SETTINGS.focusReadingEnabled; break;
    case StyleRow::Hyphenation: value = SETTINGS.hyphenationEnabled; break;
    case StyleRow::EmbeddedStyle: value = SETTINGS.embeddedStyle; break;
    case StyleRow::AntiAliasing: value = SETTINGS.textAntiAliasing; break;
    default: return {};
  }
  return value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
}

bool TextSettingsActivity::focusedRowHasNoPreview() const {
  if (ringPos() == 0 || tab_ != Tab::Style) return false;
  const auto row = static_cast<StyleRow>(ringPos() - 1);
  return row != StyleRow::FocusReading;
}

void TextSettingsActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  activeNav().selected = onTabBar ? 0 : activeNav().selected;
  activeNav().followOnBuild = true;
  requestUpdate();
}

void TextSettingsActivity::stepTab(const int direction) { switchTab(direction); }

int TextSettingsActivity::listCount() const {
  if (tab_ == Tab::Family) return static_cast<int>(fonts_.size());
  if (tab_ == Tab::Size) return static_cast<int>(sizes_.size());
  if (tab_ == Tab::Layout) return static_cast<int>(LayoutRow::Count);
  return static_cast<int>(StyleRow::Count);
}
