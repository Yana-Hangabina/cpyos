// Host check for the publish-outcome rules (src/util/FilePublishRules.h).
//
// The table is pinned two ways on purpose. The literal cases below are the six
// situations the field actually produces, each written out by hand; the property
// sweep then checks a statement that must hold for all 32 combinations, so a
// mutation cannot hide by agreeing with the implementation's own shape.
//
// The property that matters most is the one 0.8.2 got wrong: a byte-for-byte
// overwrite is a *success*, and must never be reported to the user as a storage
// failure.

#include <cstdio>
#include <cstring>

#include "util/FilePublishRules.h"

namespace {

int failures = 0;
int checks = 0;

void check(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

using file_publish::Attempts;
using file_publish::classify;
using file_publish::Outcome;
using file_publish::succeeded;

Attempts make(const bool source, const bool dest, const bool remove, const bool rename, const bool copy) {
  Attempts a;
  a.sourcePresent = source;
  a.destPresent = dest;
  a.removeOk = remove;
  a.renameOk = rename;
  a.copyOk = copy;
  return a;
}

void expect(const Attempts& a, const Outcome want, const char* what) {
  ++checks;
  const Outcome got = classify(a);
  if (got != want) {
    ++failures;
    std::printf("FAIL: %s (got %s)\n", what, file_publish::name(got));
  }
}

}  // namespace

int main() {
  // --- the six field cases, written out by hand -------------------------------
  // Fresh install: nothing on the card, the rename just moves the file.
  expect(make(true, false, true, true, false), Outcome::Renamed, "fresh install renames into place");
  // Upgrade of an existing face: the old file was deleted and the rename worked.
  expect(make(true, true, true, true, false), Outcome::Replaced, "upgrade replaces the old file");
  // The 0.8.2 font-pack case: the destination would not go away, so the bytes
  // were copied over it. This is a success, not a storage error.
  expect(make(true, true, false, false, true), Outcome::CopiedOver, "copy-over of a stuck destination succeeds");
  // Rename refused even though the destination was deleted (FAT, damaged entry).
  expect(make(true, true, true, false, true), Outcome::CopiedOver, "refused rename falls back to the copy");
  // Neither route worked and a destination is still sitting there.
  expect(make(true, true, false, false, false), Outcome::DestinationStuck, "undeletable destination is stuck");
  // Nothing to publish: never a success, whatever else happened.
  expect(make(false, true, true, true, true), Outcome::SourceMissing, "a missing source is never success");

  // --- properties over all 32 combinations -----------------------------------
  // Each evaluation counts, so the sweep is visible in the total: a loop that
  // stopped running would show up as a low check count rather than as a pass.
  int seen[6] = {0, 0, 0, 0, 0, 0};
  for (int bits = 0; bits < 32; ++bits) {
    const Attempts a = make(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16);
    const Outcome outcome = classify(a);
    ++seen[static_cast<int>(outcome)];
    check(!succeeded(outcome) || a.sourcePresent, "success reported without a source");
    check(!succeeded(outcome) || a.renameOk || a.copyOk, "success reported with neither rename nor copy");
    check(!(a.sourcePresent && a.renameOk) || succeeded(outcome), "a working rename was not success");
    check(!(a.sourcePresent && !a.renameOk && a.copyOk) || succeeded(outcome),
          "a working copy-over was not success");
    check(!(!a.renameOk && !a.copyOk) || !succeeded(outcome), "nothing moved the bytes yet it counted as success");
    // A stuck destination must be distinguishable from a plain copy failure:
    // they need different words to the user.
    if (!a.sourcePresent) continue;
    if (!a.renameOk && !a.copyOk) {
      const Attempts withDest = make(true, true, a.removeOk, false, false);
      const Attempts noDest = make(true, false, a.removeOk, false, false);
      check(classify(withDest) != classify(noDest), "stuck destination and plain copy failure share a label");
    }
  }
  // Positive control: every outcome must actually be reachable from the table.
  check(seen[static_cast<int>(Outcome::Renamed)] > 0, "Renamed is reachable");
  check(seen[static_cast<int>(Outcome::Replaced)] > 0, "Replaced is reachable");
  check(seen[static_cast<int>(Outcome::CopiedOver)] > 0, "CopiedOver is reachable");
  check(seen[static_cast<int>(Outcome::SourceMissing)] > 0, "SourceMissing is reachable");
  check(seen[static_cast<int>(Outcome::DestinationStuck)] > 0, "DestinationStuck is reachable");
  check(seen[static_cast<int>(Outcome::CopyFailed)] > 0, "CopyFailed is reachable");

  // Every outcome needs its own log label; a duplicated case in the switch would
  // silently merge two different field states.
  const Outcome all[] = {Outcome::Renamed, Outcome::Replaced, Outcome::CopiedOver, Outcome::SourceMissing,
                         Outcome::DestinationStuck, Outcome::CopyFailed};
  for (size_t i = 0; i < 6; ++i) {
    for (size_t j = i + 1; j < 6; ++j) {
      if (std::strcmp(file_publish::name(all[i]), file_publish::name(all[j])) == 0) {
        check(false, "two outcomes share a label");
      }
    }
  }
  check(file_publish::name(Outcome::CopiedOver) != nullptr, "CopiedOver has a label");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
