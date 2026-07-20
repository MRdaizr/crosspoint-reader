#pragma once

#include <cstdint>

namespace shushan {

constexpr uint8_t SAVE_VERSION = 1;
constexpr int16_t ENEMY_MAX_HP = 10;

enum class Phase : uint8_t { TITLE = 0, STORY = 1, BATTLE = 2, ENDING = 3 };
enum class StoryChoice : uint8_t { NONE = 0, FOLLOW_STREAM = 1, STUDY_MIST = 2 };
enum class CombatAction : uint8_t { SWORD_STRIKE = 0, GUARD = 1, SWORD_ART = 2 };
enum class ActionResult : uint8_t { INVALID = 0, HIT = 1, GUARDED = 2, ART_HIT = 3, NO_QI = 4, VICTORY = 5, DEFEATED = 6 };

struct PlayerState {
  int16_t hp = 10;
  int16_t maxHp = 10;
  int16_t qi = 5;
  int16_t maxQi = 5;
  int16_t cultivation = 1;
};

struct BattleState {
  int16_t enemyHp = ENEMY_MAX_HP;
};

struct GameState {
  uint8_t version = SAVE_VERSION;
  Phase phase = Phase::TITLE;
  StoryChoice choice = StoryChoice::NONE;
  PlayerState player;
  BattleState battle;
  PlayerState checkpointPlayer;
  BattleState checkpointBattle;
};

class Game final {
 public:
  const GameState& state() const { return gameState; }

  void newGame();
  bool restore(const GameState& savedState);
  bool choose(StoryChoice choice);
  ActionResult act(CombatAction action);

  static bool isValidState(const GameState& state);

 private:
  GameState gameState;

  void beginBattle();
  void restoreCheckpoint();
};

}  // namespace shushan
