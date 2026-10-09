#include "Epub/hyphenation/Hyphenator.h"

// These tests exercise continuation state, not dictionary word splitting.
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }
