// Host test for the pure installed-core rule in src/SystemCoreFontCheck.h.
//
// The rule decides whether a device has a usable reading core and whether the
// installer may overwrite what is on the card. Both halves have been wrong on real
// hardware: an unchecked marker write made a good face read as absent, and a size
// comparison alone made the installer delete the user's downloaded 8,105 pack. This
// test enumerates every fact combination, so a rule change cannot pass by only
// satisfying the case someone thought of.
#include "SystemCoreFontCheck.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

using system_core_font::classify;
using system_core_font::facePresent;
using system_core_font::Facts;
using system_core_font::kTierFieldSize;
using system_core_font::mayInstall;
using system_core_font::readTierLabel;
using system_core_font::TierLabel;
using system_core_font::usable;
using system_core_font::Verdict;

namespace {

int failures = 0;
int checks = 0;

void expect(const char* what, Verdict got, Verdict want) {
  ++checks;
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s: verdict %d, expected %d\n", what, static_cast<int>(got), static_cast<int>(want));
}

Facts base() {
  Facts facts;
  facts.storageReady = true;
  facts.fileExists = true;
  facts.sizeMatches = true;
  return facts;
}

TierLabel tagged(const char* text) {
  uint8_t field[kTierFieldSize] = {};
  std::memcpy(field, text, std::strlen(text));
  return readTierLabel(field);
}

void testNamedCases() {
  Facts none;
  expect("no card", classify(none), Verdict::NotInstalled);

  Facts noFile = base();
  noFile.fileExists = false;
  expect("card without the face", classify(noFile), Verdict::NotInstalled);

  Facts foreign = base();
  foreign.sizeMatches = false;
  foreign.markerPresent = true;
  foreign.markerMatches = true;
  expect("different size wins over a matching marker", classify(foreign), Verdict::ForeignSize);

  Facts ok = base();
  ok.markerPresent = true;
  ok.markerMatches = true;
  expect("marker agrees", classify(ok), Verdict::Ready);

  Facts repaired = base();
  repaired.contentReadable = true;
  repaired.contentMatches = true;
  expect("no marker, bytes agree", classify(repaired), Verdict::RepairMarker);

  Facts stale = base();
  stale.markerPresent = true;
  stale.markerMatches = false;
  stale.contentReadable = true;
  stale.contentMatches = true;
  expect("stale marker, bytes agree", classify(stale), Verdict::RepairMarker);

  Facts foreignContent = base();
  foreignContent.contentReadable = true;
  foreignContent.contentMatches = false;
  expect("same size, different bytes", classify(foreignContent), Verdict::ForeignContent);

  Facts unreadable = base();
  expect("bytes cannot be read back", classify(unreadable), Verdict::Unreadable);

  Facts unreadableStale = base();
  unreadableStale.markerPresent = true;
  unreadableStale.markerMatches = false;
  expect("stale marker and unreadable bytes", classify(unreadableStale), Verdict::Unreadable);
}

// The properties the shipped bugs violated, stated over the whole input space.
void testSafetyProperties() {
  unsigned combinations = 0;
  for (unsigned bits = 0; bits < 128u; ++bits) {
    // Every field independent. Some combinations cannot happen on a card (a marker
    // can only disagree after a size match), and the rule still has to answer each
    // one without certifying a wrong face and without promising a rewrite.
    Facts facts;
    facts.storageReady = (bits & 1u) != 0;
    facts.fileExists = (bits & 2u) != 0;
    facts.sizeMatches = (bits & 4u) != 0;
    facts.markerPresent = (bits & 8u) != 0;
    facts.markerMatches = (bits & 16u) != 0;
    facts.contentReadable = (bits & 32u) != 0;
    facts.contentMatches = (bits & 64u) != 0;
    ++combinations;
    const Verdict verdict = classify(facts);

    // (1) Never overwrite a face that is not ours.
    if ((verdict == Verdict::ForeignSize || verdict == Verdict::ForeignContent) && mayInstall(verdict)) {
      ++failures;
      std::printf("FAIL bits=%u: a foreign face may be overwritten\n", bits);
    }
    // (2) Never certify bytes that did not match the carrier.
    if (usable(verdict) && !facts.sizeMatches) {
      ++failures;
      std::printf("FAIL bits=%u: a wrong-size face was certified\n", bits);
    }
    if (verdict == Verdict::Ready && !(facts.markerPresent && facts.markerMatches)) {
      ++failures;
      std::printf("FAIL bits=%u: Ready without an agreeing marker\n", bits);
    }
    if (verdict == Verdict::RepairMarker && !(facts.sizeMatches && facts.contentReadable && facts.contentMatches)) {
      ++failures;
      std::printf("FAIL bits=%u: marker repair without verified content\n", bits);
    }
    if (verdict == Verdict::NotInstalled && (facts.storageReady && facts.fileExists)) {
      ++failures;
      std::printf("FAIL bits=%u: a present face reported as not installed\n", bits);
    }
    if (static_cast<int>(verdict) < 0 || static_cast<int>(verdict) > 5) {
      ++failures;
      std::printf("FAIL bits=%u: verdict out of range\n", bits);
    }
    // (3) Registering and installing are disjoint, and a certified face is always
    // registerable. Getting this pair wrong is how a device ends up with a wider font
    // sitting on the card that nothing reads (15.19).
    if (usable(verdict) && !facePresent(verdict)) {
      ++failures;
      std::printf("FAIL bits=%u: a certified face is not registerable\n", bits);
    }
    if (mayInstall(verdict) && facePresent(verdict)) {
      ++failures;
      std::printf("FAIL bits=%u: the rule would both write and reuse a face\n", bits);
    }
    if (facePresent(verdict) && !(facts.storageReady && facts.fileExists)) {
      ++failures;
      std::printf("FAIL bits=%u: reported a face with no file on a ready card\n", bits);
    }
  }
  ++checks;
  if (combinations != 128u) {
    ++failures;
    std::printf("FAIL exhaustive sweep covered %u combinations, expected 128\n", combinations);
  }
}

void testInstallPolicy() {
  ++checks;
  if (!mayInstall(Verdict::NotInstalled) || !mayInstall(Verdict::Unreadable)) {
    ++failures;
    std::printf("FAIL a missing or unreadable face must still be installable\n");
  }
  ++checks;
  if (mayInstall(Verdict::Ready) || mayInstall(Verdict::RepairMarker) || mayInstall(Verdict::ForeignSize) ||
      mayInstall(Verdict::ForeignContent)) {
    ++failures;
    std::printf("FAIL an existing face must not be rewritten\n");
  }
}

void testRegisterPolicy() {
  // The regression this pins: ForeignSize/ForeignContent used to leave the family out of
  // the supplement list entirely, so a device holding an 8,105 face released by an older
  // build -- or copied there by hand -- rendered tofu for glyphs that font covers, because
  // the firmware was busy refusing to overwrite it.
  const Verdict registerable[] = {Verdict::Ready, Verdict::RepairMarker, Verdict::ForeignSize,
                                  Verdict::ForeignContent};
  for (Verdict v : registerable) {
    ++checks;
    if (!facePresent(v)) {
      ++failures;
      std::printf("FAIL verdict %d must still be used as a font\n", static_cast<int>(v));
    }
  }
  const Verdict absent[] = {Verdict::NotInstalled, Verdict::Unreadable};
  for (Verdict v : absent) {
    ++checks;
    if (facePresent(v)) {
      ++failures;
      std::printf("FAIL verdict %d must not claim a face\n", static_cast<int>(v));
    }
  }
}

void expectTrue(const char* what, bool value) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

// The carrier's self-description. It exists so a device log, exp/verify_device_packs.py or a
// hexdump can say which tier a blob holds -- the published name is identical for both tiers,
// which is the shape that hid the earlier packaging bug. These cases pin that it is strict
// enough to be believed: a half-written or binary field must read as "unknown", never as a
// plausible-looking tier.
void testTierLabel() {
  expectTrue("3500 reads back", tagged("3500").known);
  expectTrue("8105 reads back", tagged("8105").known);
  const TierLabel eight = tagged("abcdefgh");
  expectTrue("a full-width label keeps its terminator", eight.known && eight.text[8] == '\0');
  expectTrue("the label is preserved verbatim", std::strcmp(eight.text, "abcdefgh") == 0);
  expectTrue("a short label is NUL terminated", std::strcmp(tagged("3500").text, "3500") == 0);

  uint8_t legacy[kTierFieldSize] = {};
  expectTrue("an untagged carrier is unknown, not a fault", !readTierLabel(legacy).known);
  expectTrue("unknown still yields a printable empty string", readTierLabel(legacy).text[0] == '\0');
  expectTrue("a null field is unknown", !readTierLabel(nullptr).known);

  uint8_t space[kTierFieldSize] = {' ', ' ', ' ', ' ', 0, 0, 0, 0};
  expectTrue("a blank field is unknown", !readTierLabel(space).known);
  uint8_t unpadded[kTierFieldSize] = {'3', '5', '0', '0', 0, 'x', 0, 0};
  expectTrue("bytes after the terminator are not silently dropped", !readTierLabel(unpadded).known);

  // Every byte value in the first position must either be a real label character or be
  // rejected outright -- nothing may decode to a non-empty text with known=false, and
  // nothing may claim a label built from a control or non-ASCII byte.
  for (unsigned value = 0; value < 256; ++value) {
    uint8_t field[kTierFieldSize] = {static_cast<uint8_t>(value), 0, 0, 0, 0, 0, 0, 0};
    const TierLabel parsed = readTierLabel(field);
    const char c = static_cast<char>(value);
    const bool printable = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
    expectTrue("first byte decides acceptance exactly", parsed.known == printable);
    expectTrue("an accepted label is non-empty", !parsed.known || parsed.text[0] != '\0');
    expectTrue("a rejected label carries no text", parsed.known || parsed.text[0] == '\0');
  }
}

}  // namespace

int main() {
  testNamedCases();
  testSafetyProperties();
  testInstallPolicy();
  testRegisterPolicy();
  testTierLabel();
  if (failures != 0) {
    std::printf("FAILED: %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf("PASS: %d checks over the core-install rule\n", checks);
  return 0;
}
