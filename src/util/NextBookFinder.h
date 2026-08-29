#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace NextBookFinder {

// Return up to maxCount supported book files that follow the current file in
// natural filename order and live in the same directory.  Only bare names are
// returned so callers can choose the destination without retaining directory
// handles or a second copy of the path.
std::vector<std::string> findNextBooks(const std::string& currentBookPath, size_t maxCount);

}  // namespace NextBookFinder
