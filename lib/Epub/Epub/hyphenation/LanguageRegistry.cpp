#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "HyphenationEmbedding.h"

#if defined(CROSSPOINT_EMBED_HYPHENATION)
#include "generated/hyph-de.trie.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fi.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-pt.trie.h"
#include "generated/hyph-ru.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"
#else
// The trie bytes are not in this image: they are read from /hyphen.dat or from the
// data partition on first use. This is the only place in lib/Epub that reaches the
// loader, and it is compiled out of every build that embeds the tries, which is what
// keeps test/hyphenation_eval and test/korean_line_breaking free of HalStorage.
#include "HyphenationStore.h"
#endif

namespace {

// Each hyphenator holds a *reference* to its descriptor, so the descriptor has to be
// an object with static lifetime that outlives the hyphenator. Where the bytes come
// from is the only thing that differs between builds: embedded builds alias the
// generated descriptor, storage builds get a mutable object the loader fills in on
// first use (see getLanguageHyphenatorForPrimaryTag below).
//
// The alias matters: a mutable *copy* would put 12 bytes per language in .data --
// 132 bytes of internal RAM plus their flash initialiser -- on builds that never
// write to them, and the C3 build has no PSRAM to trade against.
#if defined(CROSSPOINT_EMBED_HYPHENATION)
#define CROSSPOINT_HYPHENATION_PATTERNS(NAME, EMBEDDED) const SerializedHyphenationPatterns& NAME = EMBEDDED
// Never installed into, so the registry publishes no writable slot. The install path
// below is compiled out in this configuration and must not dereference this.
#define CROSSPOINT_HYPHENATION_SLOT(NAME) nullptr
#else
#define CROSSPOINT_HYPHENATION_PATTERNS(NAME, EMBEDDED) SerializedHyphenationPatterns NAME{}
#define CROSSPOINT_HYPHENATION_SLOT(NAME) &NAME
#endif

// English hyphenation patterns (3/3 minimum prefix/suffix length)
CROSSPOINT_HYPHENATION_PATTERNS(englishPatterns, en_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(frenchPatterns, fr_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(germanPatterns, de_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(russianPatterns, ru_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(spanishPatterns, es_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(italianPatterns, it_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(polishPatterns, pl_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(swedishPatterns, sv_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(ukrainianPatterns, uk_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(finnishPatterns, fi_patterns);
CROSSPOINT_HYPHENATION_PATTERNS(portuguesePatterns, pt_patterns);

#undef CROSSPOINT_HYPHENATION_PATTERNS

LanguageHyphenator englishHyphenator(englishPatterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(frenchPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(germanPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(russianPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(spanishPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(italianPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(swedishPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(ukrainianPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(polishPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator finnishHyphenator(finnishPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(portuguesePatterns, isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 11>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator, CROSSPOINT_HYPHENATION_SLOT(englishPatterns)},
                                       {"french", "fr", &frenchHyphenator, CROSSPOINT_HYPHENATION_SLOT(frenchPatterns)},
                                       {"german", "de", &germanHyphenator, CROSSPOINT_HYPHENATION_SLOT(germanPatterns)},
                                       {"russian", "ru", &russianHyphenator, CROSSPOINT_HYPHENATION_SLOT(russianPatterns)},
                                       {"spanish", "es", &spanishHyphenator, CROSSPOINT_HYPHENATION_SLOT(spanishPatterns)},
                                       {"italian", "it", &italianHyphenator, CROSSPOINT_HYPHENATION_SLOT(italianPatterns)},
                                       {"polish", "pl", &polishHyphenator, CROSSPOINT_HYPHENATION_SLOT(polishPatterns)},
                                       {"swedish", "sv", &swedishHyphenator, CROSSPOINT_HYPHENATION_SLOT(swedishPatterns)},
                                       {"ukrainian", "uk", &ukrainianHyphenator, CROSSPOINT_HYPHENATION_SLOT(ukrainianPatterns)},
                                       {"finnish", "fi", &finnishHyphenator, CROSSPOINT_HYPHENATION_SLOT(finnishPatterns)},
                                       {"portuguese", "pt", &portugueseHyphenator,
                                        CROSSPOINT_HYPHENATION_SLOT(portuguesePatterns)}}};
  return kEntries;
}

#undef CROSSPOINT_HYPHENATION_SLOT

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  if (it == allEntries.end()) return nullptr;

#if !defined(CROSSPOINT_EMBED_HYPHENATION)
  // Lazy, and one language at a time: reading an English book must not pay for the
  // 206 KB German trie. The store latches both outcomes per language, so a card with
  // no dictionary is probed once per session rather than once per section.
  //
  // Called on the layout task, from Hyphenator::setPreferredLanguage. Installing
  // into the descriptor is a plain 3-field write with no other writer, but it is not
  // atomic: layout must not be running concurrently on another task, which is
  // already true of the page cache this feeds.
  if (it->patterns->data == nullptr) {
    SerializedHyphenationPatterns loaded{};
    if (hyphenation::HyphenationStore::shared().patterns(it->primaryTag, loaded) && loaded.data != nullptr) {
      *it->patterns = loaded;
    }
  }
#endif

  // No trie bytes means no pattern breaks, and nullptr is the answer every caller
  // already handles: explicit hyphens, soft hyphens and apostrophe contractions
  // still break, and ParsedText's emergency fallback still applies. A missing
  // dictionary therefore degrades to what an unsupported language always did,
  // rather than introducing a second kind of "cannot hyphenate".
  return it->hyphenator->hasPatterns() ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
