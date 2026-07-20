#include "ShushanGameActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DynamicFont.h"

namespace {
constexpr char STORY_TEXT[] =
    "暮色沉入云灵山，你循着一声清越鹤唳走进古林。溪水映出淡青雾气，石缝间却有腥风涌动。"
    "你尚未拜入仙门，腰间只有一柄旧铁剑。此刻，是追随溪声强健筋骨，还是静观雾势凝聚真气？";
constexpr char CHOICE_FOLLOW_STREAM[] = "沿溪追踪（气血上限 +2）";
constexpr char CHOICE_STUDY_MIST[] = "凝神察雾（真气上限 +2）";
constexpr char BATTLE_TEXT[] = "青雾骤然裂开，蛇妖盘踞山石，鳞甲间闪着冷光。你横剑立在溪前。";
constexpr char ENDING_TEXT[] =
    "蛇妖伏地，云雾渐散。一位白衣女尼踏月而来，称你虽剑法粗浅，却有临危不退之心，愿引你入山修行。"
    "远处仙鹤再鸣，你的蜀山之路由此开始。";

const char* storyChoiceLabel(const int index) {
  return index == 0 ? CHOICE_FOLLOW_STREAM : CHOICE_STUDY_MIST;
}

int prepareFont(GfxRenderer& renderer, const char* text, const int fallbackFontId) {
  const int fontId = DynamicFont::fontForCjkText(renderer, text, fallbackFontId);
  if (renderer.isSdCardFont(fontId) && renderer.getFontCacheManager()) {
    renderer.getFontCacheManager()->prewarmCache(fontId, text, 0x01);
  }
  return fontId;
}

void prewarmText(GfxRenderer& renderer, const int fontId, const char* text) {
  if (renderer.isSdCardFont(fontId) && renderer.getFontCacheManager()) {
    renderer.getFontCacheManager()->prewarmCache(fontId, text, 0x01);
  }
}
}  // namespace

void ShushanGameActivity::onEnter() {
  Activity::onEnter();
  hasValidSave = store.load(savedState);
  selectedIndex = 0;
  requestUpdate(true);
}

void ShushanGameActivity::startNewGame() {
  game.newGame();
  selectedIndex = 0;
  lastAction = shushan::ActionResult::INVALID;
  saveFailed = false;
  saveGame();
  requestUpdate();
}

void ShushanGameActivity::continueGame() {
  if (!hasValidSave || !game.restore(savedState)) return;
  selectedIndex = 0;
  lastAction = shushan::ActionResult::INVALID;
  saveFailed = false;
  requestUpdate();
}

void ShushanGameActivity::confirmNewGame() {
  if (!hasValidSave) {
    startNewGame();
    return;
  }
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SHUSHAN_OVERWRITE_TITLE),
                                             tr(STR_SHUSHAN_OVERWRITE_BODY)),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) startNewGame();
      });
}

void ShushanGameActivity::saveGame() {
  saveFailed = !store.save(game.state());
  if (!saveFailed) {
    savedState = game.state();
    hasValidSave = true;
  }
}

void ShushanGameActivity::navigate(const int itemCount) {
  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void ShushanGameActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (game.state().phase != shushan::Phase::TITLE) saveGame();
    finish();
    return;
  }

  const auto phase = game.state().phase;
  if (phase == shushan::Phase::TITLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == 0) confirmNewGame();
      else if (selectedIndex == 1) continueGame();
      else finish();
      return;
    }
    navigate(3);
    return;
  }

  if (phase == shushan::Phase::STORY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const auto choice =
          selectedIndex == 0 ? shushan::StoryChoice::FOLLOW_STREAM : shushan::StoryChoice::STUDY_MIST;
      if (game.choose(choice)) {
        selectedIndex = 0;
        saveGame();
        requestUpdate();
      }
      return;
    }
    navigate(2);
    return;
  }

  if (phase == shushan::Phase::BATTLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      lastAction = game.act(static_cast<shushan::CombatAction>(selectedIndex));
      saveGame();
      requestUpdate();
      return;
    }
    navigate(3);
    return;
  }

  if (phase == shushan::Phase::ENDING && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    game = shushan::Game{};
    selectedIndex = 0;
    lastAction = shushan::ActionResult::INVALID;
    requestUpdate();
  }
}

void ShushanGameActivity::drawStatus(const int y) {
  const auto& player = game.state().player;
  char status[96];
  snprintf(status, sizeof(status), "%s %d/%d   %s %d/%d   %s %d", tr(STR_SHUSHAN_HP), player.hp, player.maxHp,
           tr(STR_SHUSHAN_QI), player.qi, player.maxQi, tr(STR_SHUSHAN_CULTIVATION), player.cultivation);
  const int fontId = prepareFont(renderer, status, UI_10_FONT_ID);
  renderer.drawText(fontId, UITheme::getInstance().getMetrics().contentSidePadding, y, status);
}

void ShushanGameActivity::renderTitle() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const int menuFont = prepareFont(renderer, tr(STR_SHUSHAN_NEW_GAME), UI_10_FONT_ID);
  prewarmText(renderer, menuFont, tr(STR_SHUSHAN_CONTINUE_GAME));
  prewarmText(renderer, menuFont, tr(STR_SHUSHAN_RETURN_EXTENSIONS));
  GUI.drawList(renderer, Rect{0, contentTop, width, contentHeight}, 3, selectedIndex,
               [this](const int index) {
                 if (index == 0) return std::string(tr(STR_SHUSHAN_NEW_GAME));
                 if (index == 1) return std::string(tr(STR_SHUSHAN_CONTINUE_GAME));
                 return std::string(tr(STR_SHUSHAN_RETURN_EXTENSIONS));
               },
               nullptr, nullptr, nullptr, false, [this](const int index) { return index == 1 && !hasValidSave; },
               menuFont);
  if (!hasValidSave) {
    const int noteFont = prepareFont(renderer, tr(STR_SHUSHAN_NO_SAVE), SMALL_FONT_ID);
    renderer.drawText(noteFont, metrics.contentSidePadding, contentTop + 105, tr(STR_SHUSHAN_NO_SAVE));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ShushanGameActivity::renderStory() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  drawStatus(contentTop);

  const int fontId = prepareFont(renderer, STORY_TEXT, UI_12_FONT_ID);
  const int textY = contentTop + renderer.getLineHeight(UI_10_FONT_ID) + 16;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int choicesHeight = 116;
  const int maxLines = std::max(1, (height - metrics.buttonHintsHeight - choicesHeight - textY) / lineHeight);
  const auto lines = renderer.wrappedText(fontId, STORY_TEXT, width - metrics.contentSidePadding * 2, maxLines);
  int y = textY;
  for (const auto& line : lines) {
    renderer.drawText(fontId, metrics.contentSidePadding, y, line.c_str());
    y += lineHeight;
  }

  const int choicesTop = height - metrics.buttonHintsHeight - choicesHeight;
  const int choiceFont = prepareFont(renderer, CHOICE_FOLLOW_STREAM, UI_10_FONT_ID);
  prewarmText(renderer, choiceFont, CHOICE_STUDY_MIST);
  GUI.drawList(renderer, Rect{0, choicesTop, width, choicesHeight}, 2, selectedIndex,
               [](const int index) { return std::string(storyChoiceLabel(index)); }, nullptr, nullptr, nullptr, false,
               nullptr, choiceFont);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ShushanGameActivity::renderBattle() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  drawStatus(contentTop);

  const int storyFont = prepareFont(renderer, BATTLE_TEXT, UI_10_FONT_ID);
  const auto lines = renderer.wrappedText(storyFont, BATTLE_TEXT, width - metrics.contentSidePadding * 2, 3);
  int y = contentTop + 38;
  for (const auto& line : lines) {
    renderer.drawText(storyFont, metrics.contentSidePadding, y, line.c_str());
    y += renderer.getLineHeight(storyFont);
  }

  char enemy[64];
  snprintf(enemy, sizeof(enemy), "%s  %d/%d", tr(STR_SHUSHAN_ENEMY), game.state().battle.enemyHp,
           shushan::ENEMY_MAX_HP);
  const int enemyFont = prepareFont(renderer, enemy, UI_12_FONT_ID);
  const auto enemyStyle = renderer.isSdCardFont(enemyFont) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
  renderer.drawText(enemyFont, metrics.contentSidePadding, y + 10, enemy, true, enemyStyle);

  const char* feedback = "";
  switch (lastAction) {
    case shushan::ActionResult::HIT: feedback = tr(STR_SHUSHAN_HIT); break;
    case shushan::ActionResult::GUARDED: feedback = tr(STR_SHUSHAN_GUARDED); break;
    case shushan::ActionResult::ART_HIT: feedback = tr(STR_SHUSHAN_ART_HIT); break;
    case shushan::ActionResult::NO_QI: feedback = tr(STR_SHUSHAN_NO_QI); break;
    case shushan::ActionResult::DEFEATED: feedback = tr(STR_SHUSHAN_DEFEATED_RETRY); break;
    default: break;
  }
  if (*feedback) {
    const int feedbackFont = prepareFont(renderer, feedback, UI_10_FONT_ID);
    renderer.drawText(feedbackFont, metrics.contentSidePadding, y + 45, feedback);
  }

  constexpr int actionCount = 3;
  const int actionsHeight = 155;
  const int actionsTop = height - metrics.buttonHintsHeight - actionsHeight;
  const int actionFont = prepareFont(renderer, tr(STR_SHUSHAN_SWORD_STRIKE), UI_10_FONT_ID);
  prewarmText(renderer, actionFont, tr(STR_SHUSHAN_GUARD));
  prewarmText(renderer, actionFont, tr(STR_SHUSHAN_SWORD_ART));
  GUI.drawList(renderer, Rect{0, actionsTop, width, actionsHeight}, actionCount, selectedIndex,
               [](const int index) {
                 if (index == 0) return std::string(tr(STR_SHUSHAN_SWORD_STRIKE));
                 if (index == 1) return std::string(tr(STR_SHUSHAN_GUARD));
                 return std::string(tr(STR_SHUSHAN_SWORD_ART));
               },
               nullptr, nullptr, nullptr, false, nullptr, actionFont);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ShushanGameActivity::renderEnding() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  drawStatus(contentTop);
  const int fontId = prepareFont(renderer, ENDING_TEXT, UI_12_FONT_ID);
  const auto lines = renderer.wrappedText(fontId, ENDING_TEXT, width - metrics.contentSidePadding * 2, 10);
  int y = contentTop + 44;
  for (const auto& line : lines) {
    renderer.drawText(fontId, metrics.contentSidePadding, y, line.c_str());
    y += renderer.getLineHeight(fontId);
  }
  const int victoryFont = prepareFont(renderer, tr(STR_SHUSHAN_VICTORY), UI_12_FONT_ID);
  const auto victoryStyle = renderer.isSdCardFont(victoryFont) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
  renderer.drawCenteredText(victoryFont, y + 30, tr(STR_SHUSHAN_VICTORY), true, victoryStyle);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SHUSHAN_RETURN_TITLE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ShushanGameActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_SHUSHAN_GAME));

  switch (game.state().phase) {
    case shushan::Phase::TITLE: renderTitle(); break;
    case shushan::Phase::STORY: renderStory(); break;
    case shushan::Phase::BATTLE: renderBattle(); break;
    case shushan::Phase::ENDING: renderEnding(); break;
  }
  if (saveFailed) {
    const int errorFont = prepareFont(renderer, tr(STR_SHUSHAN_SAVE_FAILED), SMALL_FONT_ID);
    renderer.drawCenteredText(errorFont, renderer.getScreenHeight() - 55, tr(STR_SHUSHAN_SAVE_FAILED));
  }
  renderer.displayBuffer();
}
