#include "LanguageRegistry.h"

#include <algorithm>

#include "HyphenationCommon.h"
#ifndef CROSSPOINT_HYPHENATION_EN
#define CROSSPOINT_HYPHENATION_EN 1
#endif
#ifndef CROSSPOINT_HYPHENATION_DE
#define CROSSPOINT_HYPHENATION_DE 0
#endif
#ifndef CROSSPOINT_HYPHENATION_ES
#define CROSSPOINT_HYPHENATION_ES 0
#endif
#ifndef CROSSPOINT_HYPHENATION_FI
#define CROSSPOINT_HYPHENATION_FI 0
#endif
#ifndef CROSSPOINT_HYPHENATION_FR
#define CROSSPOINT_HYPHENATION_FR 0
#endif
#ifndef CROSSPOINT_HYPHENATION_IT
#define CROSSPOINT_HYPHENATION_IT 0
#endif
#ifndef CROSSPOINT_HYPHENATION_PL
#define CROSSPOINT_HYPHENATION_PL 0
#endif
#ifndef CROSSPOINT_HYPHENATION_RU
#define CROSSPOINT_HYPHENATION_RU 0
#endif
#ifndef CROSSPOINT_HYPHENATION_SV
#define CROSSPOINT_HYPHENATION_SV 0
#endif
#ifndef CROSSPOINT_HYPHENATION_UK
#define CROSSPOINT_HYPHENATION_UK 0
#endif

#if CROSSPOINT_HYPHENATION_EN
#include "generated/hyph-en.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_DE
#include "generated/hyph-de.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_ES
#include "generated/hyph-es.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_FI
#include "generated/hyph-fi.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_FR
#include "generated/hyph-fr.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_IT
#include "generated/hyph-it.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_PL
#include "generated/hyph-pl.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_RU
#include "generated/hyph-ru.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_SV
#include "generated/hyph-sv.trie.h"
#endif
#if CROSSPOINT_HYPHENATION_UK
#include "generated/hyph-uk.trie.h"
#endif

namespace {

// Chinese and Japanese use the renderer's CJK character-boundary rules and do
// not require Liang hyphenation tries. The optional tries below are for
// alphabetic languages only.

// English hyphenation patterns (3/3 minimum prefix/suffix length)
#if CROSSPOINT_HYPHENATION_EN
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
#endif
#if CROSSPOINT_HYPHENATION_FR
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_DE
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_RU
LanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);
#endif
#if CROSSPOINT_HYPHENATION_ES
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_IT
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_SV
LanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_UK
LanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);
#endif
#if CROSSPOINT_HYPHENATION_PL
LanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);
#endif
#if CROSSPOINT_HYPHENATION_FI
LanguageHyphenator finnishHyphenator(fi_patterns, isLatinLetter, toLowerLatin);
#endif

#if !CROSSPOINT_HYPHENATION_EN && !CROSSPOINT_HYPHENATION_DE && !CROSSPOINT_HYPHENATION_ES && \
    !CROSSPOINT_HYPHENATION_FI && !CROSSPOINT_HYPHENATION_FR && !CROSSPOINT_HYPHENATION_IT && \
    !CROSSPOINT_HYPHENATION_PL && !CROSSPOINT_HYPHENATION_RU && !CROSSPOINT_HYPHENATION_SV && \
    !CROSSPOINT_HYPHENATION_UK
#error "At least one hyphenation language must be enabled"
#endif

constexpr LanguageEntry kEntries[] = {
#if CROSSPOINT_HYPHENATION_EN
    {"english", "en", &englishHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_FR
    {"french", "fr", &frenchHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_DE
    {"german", "de", &germanHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_RU
    {"russian", "ru", &russianHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_ES
    {"spanish", "es", &spanishHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_IT
    {"italian", "it", &italianHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_PL
    {"polish", "pl", &polishHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_SV
    {"swedish", "sv", &swedishHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_UK
    {"ukrainian", "uk", &ukrainianHyphenator},
#endif
#if CROSSPOINT_HYPHENATION_FI
    {"finnish", "fi", &finnishHyphenator},
#endif
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
