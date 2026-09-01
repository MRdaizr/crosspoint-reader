#include "LanguageRegistry.h"

#include <algorithm>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

namespace {

// Chinese and Japanese use the renderer's CJK character-boundary rules and do
// not require Liang hyphenation tries. English is the only alphabetic language
// with embedded Liang patterns in the slim firmware.

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

constexpr LanguageEntry kEntries[] = {
    {"english", "en", &englishHyphenator},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto it = std::find_if(kEntries, kEntries + kEntryCount,
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != kEntries + kEntryCount) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  return LanguageEntryView{kEntries, kEntryCount};
}
