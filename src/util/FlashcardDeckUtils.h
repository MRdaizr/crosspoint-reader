#pragma once

#include <cstddef>

namespace FlashcardDeckUtils {

// Keep the existing visible location first; the hidden location is an additional source.
inline constexpr const char* ROOTS[] = {"/flashcards", "/.flashcards"};
inline constexpr size_t ROOT_COUNT = sizeof(ROOTS) / sizeof(ROOTS[0]);

}  // namespace FlashcardDeckUtils
