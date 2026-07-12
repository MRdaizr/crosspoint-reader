#include "LanguageRegistry.h"

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
constexpr LanguageEntry ENTRIES[] = {{"english", "en", &englishHyphenator}};

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  return primaryTag == "en" ? &englishHyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  return LanguageEntryView{ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0])};
}
