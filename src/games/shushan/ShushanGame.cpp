#include "ShushanGame.h"

#include <algorithm>

namespace shushan {

namespace {
constexpr int16_t ENEMY_DAMAGE = 2;
constexpr int16_t SWORD_ART_COST = 2;
constexpr int16_t SWORD_ART_DAMAGE = 5;

bool validPlayer(const PlayerState& player) {
  return player.maxHp >= 1 && player.maxHp <= 99 && player.hp >= 0 && player.hp <= player.maxHp &&
         player.maxQi >= 0 && player.maxQi <= 99 && player.qi >= 0 && player.qi <= player.maxQi &&
         player.cultivation >= 1 && player.cultivation <= 20;
}
}  // namespace

void Game::newGame() {
  gameState = {};
  gameState.phase = Phase::STORY;
}

bool Game::restore(const GameState& savedState) {
  if (!isValidState(savedState)) return false;
  gameState = savedState;
  return true;
}

bool Game::choose(const StoryChoice choice) {
  if (gameState.phase != Phase::STORY ||
      (choice != StoryChoice::FOLLOW_STREAM && choice != StoryChoice::STUDY_MIST)) {
    return false;
  }

  gameState.choice = choice;
  if (choice == StoryChoice::FOLLOW_STREAM) {
    gameState.player.maxHp += 2;
    gameState.player.hp = gameState.player.maxHp;
  } else if (choice == StoryChoice::STUDY_MIST) {
    gameState.player.maxQi += 2;
    gameState.player.qi = gameState.player.maxQi;
  }
  beginBattle();
  return true;
}

void Game::beginBattle() {
  gameState.phase = Phase::BATTLE;
  gameState.battle.enemyHp = ENEMY_MAX_HP;
  gameState.checkpointPlayer = gameState.player;
  gameState.checkpointBattle = gameState.battle;
}

void Game::restoreCheckpoint() {
  gameState.player = gameState.checkpointPlayer;
  gameState.battle = gameState.checkpointBattle;
  gameState.phase = Phase::BATTLE;
}

ActionResult Game::act(const CombatAction action) {
  if (gameState.phase != Phase::BATTLE) return ActionResult::INVALID;

  int16_t damage = 0;
  bool guarding = false;
  ActionResult result = ActionResult::INVALID;
  switch (action) {
    case CombatAction::SWORD_STRIKE:
      damage = 2 + gameState.player.cultivation;
      result = ActionResult::HIT;
      break;
    case CombatAction::GUARD:
      guarding = true;
      result = ActionResult::GUARDED;
      break;
    case CombatAction::SWORD_ART:
      if (gameState.player.qi < SWORD_ART_COST) return ActionResult::NO_QI;
      gameState.player.qi -= SWORD_ART_COST;
      damage = SWORD_ART_DAMAGE;
      result = ActionResult::ART_HIT;
      break;
    default:
      return ActionResult::INVALID;
  }

  gameState.battle.enemyHp = std::max<int16_t>(0, gameState.battle.enemyHp - damage);
  if (gameState.battle.enemyHp == 0) {
    gameState.phase = Phase::ENDING;
    return ActionResult::VICTORY;
  }

  const int16_t incomingDamage = guarding ? (ENEMY_DAMAGE + 1) / 2 : ENEMY_DAMAGE;
  gameState.player.hp = std::max<int16_t>(0, gameState.player.hp - incomingDamage);
  if (gameState.player.hp == 0) {
    restoreCheckpoint();
    return ActionResult::DEFEATED;
  }
  return result;
}

bool Game::isValidState(const GameState& state) {
  if (state.version != SAVE_VERSION || !validPlayer(state.player)) return false;
  if (state.phase != Phase::STORY && state.phase != Phase::BATTLE && state.phase != Phase::ENDING) return false;
  if (state.choice != StoryChoice::NONE && state.choice != StoryChoice::FOLLOW_STREAM &&
      state.choice != StoryChoice::STUDY_MIST) return false;
  if (state.battle.enemyHp < 0 || state.battle.enemyHp > ENEMY_MAX_HP) return false;

  if (state.phase == Phase::STORY) return state.choice == StoryChoice::NONE;
  if (state.choice == StoryChoice::NONE || !validPlayer(state.checkpointPlayer) ||
      state.checkpointBattle.enemyHp != ENEMY_MAX_HP) {
    return false;
  }
  if (state.phase == Phase::BATTLE) return state.battle.enemyHp > 0;
  return state.battle.enemyHp == 0;
}

}  // namespace shushan
