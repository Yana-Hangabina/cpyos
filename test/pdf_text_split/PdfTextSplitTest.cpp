#include "pdf/PdfTextSplit.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

static int checks = 0;
static int failures = 0;
#define CHECK(c)                              \
  do {                                        \
    ++checks;                                 \
    if (!(c)) {                               \
      ++failures;                             \
      printf("FAIL line %d: %s\n", __LINE__, #c); \
    }                                         \
  } while (0)

using Strs = std::vector<std::string>;

// Compare the split result against expected paragraphs, without putting a
// braced init list inside the CHECK macro.
static bool splitsTo(const char* text, std::initializer_list<const char*> want) {
  const std::string in(text);
  const Strs got = pdf_text::splitParagraphs(in.data(), in.size());
  Strs expected;
  for (const char* w : want) expected.emplace_back(w);
  return got == expected;
}

static size_t splitCount(const char* text) {
  const std::string in(text);
  return pdf_text::splitParagraphs(in.data(), in.size()).size();
}

static Strs splitOf(const char* text) {
  const std::string in(text);
  return pdf_text::splitParagraphs(in.data(), in.size());
}

int main() {
  CHECK(splitsTo("hello", {"hello"}));
  CHECK(splitsTo("one\n\ntwo", {"one", "two"}));

  // Hard line breaks inside a block stay INSIDE the paragraph: this is the
  // contract pdf_merge relies on when it flattens them again.
  CHECK(splitsTo("a\nb\nc", {"a\nb\nc"}));
  CHECK(splitsTo("a\nb\n\nc", {"a\nb", "c"}));

  // The tail must survive when the buffer ends without a blank line.
  CHECK(splitsTo("a\n\nb\nlast", {"a", "b\nlast"}));

  // A lone trailing newline is content, not a separator (the i + 1 < n guard).
  CHECK(splitsTo("a\nb\n", {"a\nb\n"}));

  // A trailing blank line yields no empty paragraph.
  CHECK(splitsTo("a\n\nb\n\n", {"a", "b"}));

  // Runs of blank lines still split exactly once.
  CHECK(splitCount("a\n\n\nb") == 2);
  CHECK(splitCount("a\n\n\n\nb") == 2);
  const Strs triple = splitOf("a\n\n\nb");
  CHECK(triple.size() == 2 && triple[0] == "a");
  CHECK(triple.size() == 2 && (triple[1] == "\nb" || triple[1] == "b"));

  // Empty and blank-line-only pages produce nothing.
  CHECK(splitCount("") == 0);
  CHECK(splitCount("\n\n") == 0);
  CHECK(splitCount("\n\n\n\n") == 0);

  // Measured asymmetry, kept as-is on purpose: a lone "\n" is not a blank
  // line, so it is content and survives as one whitespace paragraph -- the
  // same verbatim rule that keeps "a\nb\n" ending in a newline. Nothing
  // renders it: buildReflow rejects pages under 200 bytes of extracted text,
  // and with the merge option on, trimLeft/trimRight drop the entry. The
  // caller owns whitespace filtering; the splitter owns boundaries.
  CHECK(splitsTo("\n", {"\n"}));

  // Bytes are copied verbatim, including multi-byte UTF-8 next to a break.
  const Strs cjk = splitOf("\xe6\x9c\xac\xe4\xb9\xa6\n\n\xe4\xbd\xa0\xe6\x88\x90");
  CHECK(cjk.size() == 2);
  CHECK(cjk.size() == 2 && cjk[0] == "\xe6\x9c\xac\xe4\xb9\xa6");
  CHECK(cjk.size() == 2 && cjk[1] == "\xe4\xbd\xa0\xe6\x88\x90");

  // Null buffer is a no-op, not a crash.
  CHECK(pdf_text::splitParagraphs(nullptr, 10).empty());

  printf("pdf text split host regression: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
