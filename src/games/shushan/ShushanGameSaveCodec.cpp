#include "ShushanGameSaveCodec.h"

#include <ArduinoJson.h>

namespace shushan {

namespace {
void writePlayer(JsonObject object, const PlayerState& player) {
  object["hp"] = player.hp;
  object["maxHp"] = player.maxHp;
  object["qi"] = player.qi;
  object["maxQi"] = player.maxQi;
  object["cultivation"] = player.cultivation;
}

PlayerState readPlayer(JsonObjectConst object) {
  PlayerState player;
  player.hp = object["hp"] | -1;
  player.maxHp = object["maxHp"] | -1;
  player.qi = object["qi"] | -1;
  player.maxQi = object["maxQi"] | -1;
  player.cultivation = object["cultivation"] | -1;
  return player;
}
}  // namespace

bool encodeSave(const GameState& state, char* output, const size_t outputSize, size_t& written) {
  written = 0;
  if (!output || outputSize == 0 || !Game::isValidState(state)) return false;

  JsonDocument document;
  document["version"] = state.version;
  document["phase"] = static_cast<uint8_t>(state.phase);
  document["choice"] = static_cast<uint8_t>(state.choice);
  writePlayer(document["player"].to<JsonObject>(), state.player);
  document["battle"]["enemyHp"] = state.battle.enemyHp;
  writePlayer(document["checkpointPlayer"].to<JsonObject>(), state.checkpointPlayer);
  document["checkpointBattle"]["enemyHp"] = state.checkpointBattle.enemyHp;

  const size_t required = measureJson(document);
  if (required + 1 > outputSize) return false;
  written = serializeJson(document, output, outputSize);
  return written == required;
}

bool decodeSave(const char* input, const size_t inputSize, GameState& state) {
  if (!input || inputSize == 0) return false;
  JsonDocument document;
  if (deserializeJson(document, input, inputSize)) return false;

  GameState decoded;
  decoded.version = document["version"] | 0;
  decoded.phase = static_cast<Phase>(document["phase"] | 255);
  decoded.choice = static_cast<StoryChoice>(document["choice"] | 255);
  decoded.player = readPlayer(document["player"].as<JsonObjectConst>());
  decoded.battle.enemyHp = document["battle"]["enemyHp"] | -1;
  decoded.checkpointPlayer = readPlayer(document["checkpointPlayer"].as<JsonObjectConst>());
  decoded.checkpointBattle.enemyHp = document["checkpointBattle"]["enemyHp"] | -1;

  if (!Game::isValidState(decoded)) return false;
  state = decoded;
  return true;
}

}  // namespace shushan
