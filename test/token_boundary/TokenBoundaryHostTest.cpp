// Host regression for TokenBoundary::isJustifiableGap (pure header, no HAL / no gtest).
//
// The gap rule sees only `continues` (the boundary before this token is attached) plus a
// caller-derived `isStretchableSpace` (this token is a literal space AND the token after it
// keeps the continuation). These checks pin the truth table and then model the derivation
// over small token runs, including the hyphen case the fix was written for.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "TokenBoundary.h"

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// continues[i] describes the boundary BEFORE token i: true when token i is attached to the
// token before it. Index 0 is unused (a line never starts mid-attachment for this model).
bool stretchableAt(const std::vector<std::string>& words, const std::vector<bool>& continues,
                   const size_t boundaryIdx) {
  return words[boundaryIdx] == " " && boundaryIdx + 1 < continues.size() && continues[boundaryIdx + 1];
}

size_t countJustifiableGaps(const std::vector<std::string>& words, const std::vector<bool>& continues) {
  size_t gaps = 0;
  for (size_t i = 1; i < words.size(); ++i) {
    if (TokenBoundary::isJustifiableGap(continues[i], stretchableAt(words, continues, i))) ++gaps;
  }
  return gaps;
}

// The pre-8b2ba5e9 rule, kept here only to show which layout changed.
bool oldRule(const bool continues, const bool noSpaceBefore, const bool isSpaceToken) {
  return !continues || (isSpaceToken && !noSpaceBefore);
}

}  // namespace

int main() {
  // Truth table. A non-continuing boundary is an ordinary breakable gap regardless of what
  // the token is; a continuing boundary only stretches when it is a stretchable space.
  check(TokenBoundary::isJustifiableGap(false, false), "ordinary word gap must stretch");
  check(TokenBoundary::isJustifiableGap(false, true), "breakable gap on a space token must stretch");
  check(!TokenBoundary::isJustifiableGap(true, false), "non-space attachment must not stretch");
  check(TokenBoundary::isJustifiableGap(true, true), "attached space token must stretch");
  check(TokenBoundary::allowsBreak(false, false), "ordinary word gap stays breakable");
  check(!TokenBoundary::allowsBreak(true, false), "attachment stays unbreakable");

  // "200<nbsp>km to go": the no-break space is its own token, attached on both sides.
  //   tokens   : 200  nbsp  km    to    go
  //   continues:  -    T     T     F     F
  // Only the boundary before the space token (the nb-s gap) plus the two ordinary word gaps
  // are counted; the space-to-km attachment is not a second gap.
  {
    const std::vector<std::string> words = {"200", " ", "km", "to", "go"};
    const std::vector<bool> continues = {false, true, true, false, false};
    check(stretchableAt(words, continues, 1), "nbsp token is stretchable when the next token continues");
    check(!stretchableAt(words, continues, 2), "the attached word is not a stretchable space");
    check(countJustifiableGaps(words, continues) == 3, "nbsp contributes one gap, not two");
  }

  // "Quadrat-<space>kilometer": the space after a visible hyphen is now justified; the old
  // rule dropped it because noSpaceBefore was set on the hyphen boundary.
  {
    const std::vector<std::string> words = {"Quadrat-", " ", "kilometer"};
    const std::vector<bool> continues = {false, true, true};
    check(countJustifiableGaps(words, continues) == 1, "space after a hyphen is a justified gap");
    check(oldRule(/*continues=*/true, /*noSpaceBefore=*/true, /*isSpaceToken=*/true) == false,
          "old rule disagreed here (documents the change)");
  }

  // A bare hyphen continuation is attached text, not a gap.
  {
    const std::vector<std::string> words = {"Quadrat-", "kilometer"};
    const std::vector<bool> continues = {false, true, false};
    check(!stretchableAt(words, continues, 1), "a hyphen is not a stretchable space");
    check(countJustifiableGaps(words, continues) == 0, "hyphen continuation is not a gap");
  }

  // An ellipsis continuation has the same shape as a hyphen: the ellipsis stays attached to
  // the following word, so the boundary is not a gap.
  {
    const std::vector<std::string> words = {"wait\xE2\x80\xA6", "more"};
    const std::vector<bool> continues = {false, true, false};
    check(!stretchableAt(words, continues, 1), "an ellipsis is not a stretchable space");
    check(countJustifiableGaps(words, continues) == 0, "ellipsis continuation is not a gap");
  }

  // A space token that ends the line has no following token, so it is not stretchable.
  {
    const std::vector<std::string> words = {"end", " "};
    const std::vector<bool> continues = {false, true};
    check(!stretchableAt(words, continues, 1), "a line-final space token is not stretchable");
    check(countJustifiableGaps(words, continues) == 0, "a line-final space token is not a gap");
  }

  // A carried no-break space stripped at a wrapped line start leaves the next word as the
  // first token: a two-word line has exactly one (ordinary) gap.
  {
    const std::vector<std::string> words = {"km", "to"};
    const std::vector<bool> continues = {false, false, false};
    check(countJustifiableGaps(words, continues) == 1, "stripped line start lays out one gap");
  }

  if (failures == 0) std::printf("PASS: token boundary rule (0 failures)\n");
  return failures == 0 ? 0 : 1;
}
