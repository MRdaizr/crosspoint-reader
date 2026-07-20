#pragma once

#include <cstddef>

#include "ShushanGame.h"

namespace shushan {

bool encodeSave(const GameState& state, char* output, size_t outputSize, size_t& written);
bool decodeSave(const char* input, size_t inputSize, GameState& state);

}  // namespace shushan
