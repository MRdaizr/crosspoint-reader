#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "games/shushan/ShushanGame.h"
#include "games/shushan/ShushanGameSaveCodec.h"

using namespace shushan;

namespace {
Game battleGame(StoryChoice choice = StoryChoice::FOLLOW_STREAM) {
  Game game;
  game.newGame();
  EXPECT_TRUE(game.choose(choice));
  return game;
}
}  // namespace

TEST(ShushanGame, NewGameStartsStory) {
  Game game;
  game.newGame();
  EXPECT_EQ(game.state().phase, Phase::STORY);
  EXPECT_EQ(game.state().player.hp, 10);
  EXPECT_EQ(game.state().choice, StoryChoice::NONE);
  EXPECT_TRUE(Game::isValidState(game.state()));
}

TEST(ShushanGame, ChoicesApplyBonusAndCreateCheckpoint) {
  Game stream = battleGame(StoryChoice::FOLLOW_STREAM);
  EXPECT_EQ(stream.state().player.maxHp, 12);
  EXPECT_EQ(stream.state().checkpointPlayer.maxHp, 12);
  EXPECT_EQ(stream.state().battle.enemyHp, ENEMY_MAX_HP);

  Game mist = battleGame(StoryChoice::STUDY_MIST);
  EXPECT_EQ(mist.state().player.maxQi, 7);
  EXPECT_EQ(mist.state().checkpointPlayer.maxQi, 7);
}

TEST(ShushanGame, InvalidChoiceDoesNotChangeState) {
  Game game;
  game.newGame();
  const GameState before = game.state();

  EXPECT_FALSE(game.choose(static_cast<StoryChoice>(99)));
  EXPECT_EQ(game.state().phase, before.phase);
  EXPECT_EQ(game.state().choice, before.choice);
  EXPECT_EQ(game.state().player.hp, before.player.hp);
}

TEST(ShushanGame, CombatActionsAreDeterministic) {
  Game game = battleGame();
  EXPECT_EQ(game.act(CombatAction::SWORD_STRIKE), ActionResult::HIT);
  EXPECT_EQ(game.state().battle.enemyHp, 7);
  EXPECT_EQ(game.state().player.hp, 10);

  EXPECT_EQ(game.act(CombatAction::GUARD), ActionResult::GUARDED);
  EXPECT_EQ(game.state().battle.enemyHp, 7);
  EXPECT_EQ(game.state().player.hp, 9);

  EXPECT_EQ(game.act(CombatAction::SWORD_ART), ActionResult::ART_HIT);
  EXPECT_EQ(game.state().battle.enemyHp, 2);
  EXPECT_EQ(game.state().player.qi, 3);
}

TEST(ShushanGame, InsufficientQiDoesNotUseTurn) {
  Game game = battleGame();
  GameState state = game.state();
  state.player.qi = 1;
  ASSERT_TRUE(game.restore(state));

  EXPECT_EQ(game.act(CombatAction::SWORD_ART), ActionResult::NO_QI);
  EXPECT_EQ(game.state().player.hp, state.player.hp);
  EXPECT_EQ(game.state().battle.enemyHp, state.battle.enemyHp);
}

TEST(ShushanGame, InvalidCombatActionDoesNotUseTurn) {
  Game game = battleGame();
  const GameState before = game.state();

  EXPECT_EQ(game.act(static_cast<CombatAction>(99)), ActionResult::INVALID);
  EXPECT_EQ(game.state().player.hp, before.player.hp);
  EXPECT_EQ(game.state().player.qi, before.player.qi);
  EXPECT_EQ(game.state().battle.enemyHp, before.battle.enemyHp);
}

TEST(ShushanGame, VictoryEntersEnding) {
  Game game = battleGame();
  EXPECT_EQ(game.act(CombatAction::SWORD_ART), ActionResult::ART_HIT);
  EXPECT_EQ(game.act(CombatAction::SWORD_ART), ActionResult::VICTORY);
  EXPECT_EQ(game.state().phase, Phase::ENDING);
  EXPECT_EQ(game.state().battle.enemyHp, 0);
}

TEST(ShushanGame, DefeatRestoresBattleCheckpoint) {
  Game game = battleGame();
  GameState state = game.state();
  state.player.hp = 1;
  ASSERT_TRUE(game.restore(state));

  EXPECT_EQ(game.act(CombatAction::SWORD_STRIKE), ActionResult::DEFEATED);
  EXPECT_EQ(game.state().phase, Phase::BATTLE);
  EXPECT_EQ(game.state().player.hp, game.state().checkpointPlayer.hp);
  EXPECT_EQ(game.state().battle.enemyHp, ENEMY_MAX_HP);
}

TEST(ShushanSave, RoundTripsValidState) {
  Game game = battleGame(StoryChoice::STUDY_MIST);
  ASSERT_EQ(game.act(CombatAction::SWORD_STRIKE), ActionResult::HIT);

  char json[768] = {};
  size_t written = 0;
  ASSERT_TRUE(encodeSave(game.state(), json, sizeof(json), written));
  ASSERT_GT(written, 0u);
  ASSERT_LT(written, sizeof(json));

  GameState decoded;
  ASSERT_TRUE(decodeSave(json, written, decoded));
  EXPECT_EQ(decoded.phase, game.state().phase);
  EXPECT_EQ(decoded.choice, game.state().choice);
  EXPECT_EQ(decoded.player.hp, game.state().player.hp);
  EXPECT_EQ(decoded.player.maxQi, game.state().player.maxQi);
  EXPECT_EQ(decoded.battle.enemyHp, game.state().battle.enemyHp);
}

TEST(ShushanSave, RejectsCorruptAndFutureSaves) {
  GameState decoded;
  constexpr char corrupt[] = "{not-json";
  EXPECT_FALSE(decodeSave(corrupt, sizeof(corrupt) - 1, decoded));

  Game game = battleGame();
  char json[768] = {};
  size_t written = 0;
  ASSERT_TRUE(encodeSave(game.state(), json, sizeof(json), written));
  std::string future(json, written);
  const size_t version = future.find("\"version\":1");
  ASSERT_NE(version, std::string::npos);
  future[version + strlen("\"version\":")] = '2';
  EXPECT_FALSE(decodeSave(future.data(), future.size(), decoded));
}
