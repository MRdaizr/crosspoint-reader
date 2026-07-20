#include "ShushanGameStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "ShushanGameSaveCodec.h"

namespace shushan {

namespace {
constexpr char SAVE_PATH[] = "/.crosspoint/shushan_game.json";
constexpr size_t SAVE_BUFFER_SIZE = 768;
}  // namespace

bool GameStore::load(GameState& state) const {
  if (!Storage.exists(SAVE_PATH)) return false;
  char buffer[SAVE_BUFFER_SIZE] = {};
  const size_t bytesRead = Storage.readFileToBuffer(SAVE_PATH, buffer, sizeof(buffer));
  if (bytesRead == 0 || bytesRead >= sizeof(buffer) - 1 || !decodeSave(buffer, bytesRead, state)) {
    LOG_ERR("SHU", "Invalid Shushan save file");
    return false;
  }
  return true;
}

bool GameStore::save(const GameState& state) const {
  char buffer[SAVE_BUFFER_SIZE] = {};
  size_t written = 0;
  if (!encodeSave(state, buffer, sizeof(buffer), written)) {
    LOG_ERR("SHU", "Could not encode Shushan save");
    return false;
  }
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(SAVE_PATH, String(buffer))) {
    LOG_ERR("SHU", "Could not write Shushan save");
    return false;
  }
  return true;
}

}  // namespace shushan
