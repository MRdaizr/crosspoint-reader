#pragma once

#include "ShushanGame.h"

namespace shushan {

class GameStore final {
 public:
  bool load(GameState& state) const;
  bool save(const GameState& state) const;
};

}  // namespace shushan
