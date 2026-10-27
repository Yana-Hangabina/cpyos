#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const LanguageHyphenator* hyphenator;
  // The descriptor this language's hyphenator holds a reference to, so a build that
  // loads tries from storage can install them after construction. nullptr in an
  // embedded build, where the descriptor is a const alias for the generated one and
  // nothing ever writes to it -- the install path is compiled out there and must not
  // dereference this.
  SerializedHyphenationPatterns* patterns;
};

struct LanguageEntryView {
  const LanguageEntry* data;
  size_t size;

  const LanguageEntry* begin() const { return data; }
  const LanguageEntry* end() const { return data + size; }
};

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr").
// Returns nullptr when the tag is unknown AND when the language has no trie behind
// it, which in a storage-loading build means the dictionary is missing, unreadable
// or corrupt. Callers already treat nullptr as "no pattern breaks", so a missing
// dictionary degrades to the behaviour an unsupported language always had.
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);

// Exposes the list of supported languages primarily for tooling/tests.
LanguageEntryView getLanguageEntries();
