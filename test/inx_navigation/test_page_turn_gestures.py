#!/usr/bin/env python3
"""Page-turn gesture settings: stored values, legacy migration, tap classification.

Upstream added two regressions here (5eb498b0 mixed-pair migration, 052d481a
"the Disabled row is not the Disabled value"). CPYOS lays the rows out
differently — plain enums whose stored value equals the row index, with
INVERTED_TAP folded at read time — so the upstream assertions do not port
verbatim. What must hold in *this* tree, and is pinned below:

  * the legacy combined `touchReaderControls` byte still splits into the pair
    (1=tap, 2=swipe, 3=inverted tap) with both directions equal;
  * a save that already carries the pair — including a mixed one such as
    next=Inverted Tap with previous=Disabled — is left untouched by that
    migration, so the odd previous value survives;
  * PAGE_TURN_GESTURE has room for the stored DISABLED value (4), which is what
    makes "Disabled" persist at all;
  * the reader's tap branch is quiet when neither direction accepts taps and
    turns the correct side when only one does, through the same
    gestureAllowsTap() + tapzones::legacyActionAt() production code the reader
    uses for its legacy-thirds hit test.

The enum, gestureAllowsTap() and the migration block are compiled from the real
sources (extracted by name, with asserts so a rename fails loudly instead of
silently testing nothing); only their collaborators are stubbed.

Run: python3 test/inx_navigation/test_page_turn_gestures.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


def brace_block(text, start):
    """Return text[start:end] spanning one balanced {...} block from `start`."""
    assert text[start] == '{', text[start:start + 40]
    depth = 0
    for index in range(start, len(text)):
        depth += (text[index] == '{') - (text[index] == '}')
        if depth == 0:
            return text[start:index + 1]
    raise ValueError('unbalanced braces')


def gesture_enum():
    header = read('src/CrossPointSettings.h')
    start = header.index('enum PAGE_TURN_GESTURE {')
    block = brace_block(header, header.index('{', start))
    values = dict(re.findall(r'(\w+)\s*=\s*(\d+)', block))
    # Assert the names/values this regression reasons about, so a reshuffle in
    # the header breaks the harness instead of the assumptions under it.
    assert values == {
        'TAP_AND_SWIPE': '0',
        'TAP_ONLY': '1',
        'SWIPE_ONLY': '2',
        'INVERTED_TAP': '3',
        'PAGE_TURN_GESTURE_DISABLED': '4',
    }, values
    return 'enum PAGE_TURN_GESTURE ' + block + ';'


def gesture_allows_tap():
    utils = read('src/activities/reader/ReaderUtils.h')
    start = utils.index('inline bool gestureAllowsTap(const uint8_t gesture) {')
    return utils[start:utils.index('\n}\n', start) + 3]


def migration_block():
    cpp = read('src/CrossPointSettings.cpp')
    start = cpp.index('  if (doc["pageTurnGesture"].isNull()')
    condition_end = cpp.index(') {\n', start) + 3
    return cpp[start:condition_end - 1] + brace_block(cpp, condition_end - 1)


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "TapZones.h"

struct CrossPointSettings {
  __ENUM__
};

__GESTURE_ALLOWS_TAP__

namespace {

// Minimal stand-in for the ArduinoJson document the migration reads: three
// optional uint8 fields keyed by name.
struct Variant {
  bool present = false;
  uint8_t value = 0;
  bool isNull() const { return !present; }
  template <typename T>
  bool is() const { return present; }
  template <typename T>
  T as() const { return static_cast<T>(value); }
};

struct Json {
  Variant pageTurn, previousPage, touchMode;
  Variant operator[](const char* key) const {
    if (std::strcmp(key, "pageTurnGesture") == 0) return pageTurn;
    if (std::strcmp(key, "previousPageGesture") == 0) return previousPage;
    if (std::strcmp(key, "touchReaderControls") == 0) return touchMode;
    return Variant{};
  }
};

// The production migration block operates on these names; keep them as they
// are so the extracted statement compiles unchanged.
static Json doc;
static uint8_t touchReaderControls = 1;
static uint8_t pageTurnGesture = 0;
static uint8_t previousPageGesture = 0;
static bool needsResave = false;

using CrossPointSettings::TAP_AND_SWIPE;
using CrossPointSettings::TAP_ONLY;
using CrossPointSettings::SWIPE_ONLY;
using CrossPointSettings::INVERTED_TAP;
using CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
using CrossPointSettings::PAGE_TURN_GESTURE_COUNT;
constexpr uint8_t TOUCH_READER_ON = 1;  // CrossPointSettings::TOUCH_READER_ON

static int failures = 0;

void expect(const bool ok, const char* what, const int got, const int want) {
  if (ok) return;
  std::printf("FAIL %s: got %d, want %d\n", what, got, want);
  ++failures;
}

void reset(uint8_t next, uint8_t prev, int legacyMode) {
  doc = Json{};
  pageTurnGesture = next;
  previousPageGesture = prev;
  touchReaderControls = TOUCH_READER_ON;
  needsResave = false;
  if (legacyMode >= 0) {
    doc.touchMode.present = true;
    doc.touchMode.value = static_cast<uint8_t>(legacyMode);
  }
}

void runMigration() {
__MIGRATION__
}

// The reader's legacy-thirds tap decision: only the direction that accepts taps
// may turn, and Inverted Tap swaps which side turns forward.
uint8_t tapAt(int x, int y, uint8_t next, uint8_t prev, bool invertedPair) {
  const int w = 1200, h = 800;
  const bool nextTaps = gestureAllowsTap(next);
  const bool prevTaps = gestureAllowsTap(prev);
  return tapzones::legacyActionAt(x, y, w, h, invertedPair, prevTaps, nextTaps, false);
}

}  // namespace

int main() {
  // 1. The stored DISABLED value is inside the enum, so a save can hold it.
  expect(PAGE_TURN_GESTURE_COUNT == 5, "enum keeps a value for Disabled", PAGE_TURN_GESTURE_COUNT, 5);

  // 2. Legacy combined modes split into an identical pair.
  const struct { int mode; uint8_t want; } legacy[] = {
      {1, TAP_ONLY}, {2, SWIPE_ONLY}, {3, INVERTED_TAP}};
  for (const auto& c : legacy) {
    reset(TAP_AND_SWIPE, TAP_AND_SWIPE, c.mode);
    runMigration();
    expect(pageTurnGesture == c.want, "legacy mode -> next gesture", pageTurnGesture, c.want);
    expect(previousPageGesture == c.want, "legacy mode -> prev gesture", previousPageGesture, c.want);
    expect(touchReaderControls == TOUCH_READER_ON, "legacy mode keeps touch on", touchReaderControls,
           TOUCH_READER_ON);
    expect(needsResave, "legacy mode asks for a resave", needsResave ? 1 : 0, 1);

    // And the migrated pair classifies like the old single mode did.
    const bool tap = gestureAllowsTap(c.want);
    const uint8_t right = tap ? static_cast<uint8_t>(tapzones::NEXT) : static_cast<uint8_t>(tapzones::NONE);
    expect(tapAt(1100, 400, pageTurnGesture, previousPageGesture, false) == right,
           "migrated pair keeps the right-edge behaviour", tapAt(1100, 400, pageTurnGesture, previousPageGesture, false),
           right);
  }

  // 3. A save that already carries a mixed pair is not touched. next=Inverted
  //    Tap with previous=Disabled is the pair from the review: the odd previous
  //    value has to survive, which is why the Disabled row exists at all.
  reset(INVERTED_TAP, PAGE_TURN_GESTURE_DISABLED, 3);
  doc.pageTurn.present = true;
  doc.previousPage.present = true;
  doc.pageTurn.value = INVERTED_TAP;
  doc.previousPage.value = PAGE_TURN_GESTURE_DISABLED;
  runMigration();
  expect(pageTurnGesture == INVERTED_TAP, "mixed pair keeps next", pageTurnGesture, INVERTED_TAP);
  expect(previousPageGesture == PAGE_TURN_GESTURE_DISABLED, "mixed pair keeps previous", previousPageGesture,
         PAGE_TURN_GESTURE_DISABLED);
  expect(!needsResave, "mixed pair needs no resave", needsResave ? 1 : 0, 0);

  // Same for Tap Only + Inverted Tap: the two directions stay independent.
  reset(TAP_ONLY, INVERTED_TAP, 1);
  doc.pageTurn.present = true;
  doc.previousPage.present = true;
  doc.pageTurn.value = TAP_ONLY;
  doc.previousPage.value = INVERTED_TAP;
  runMigration();
  expect(pageTurnGesture == TAP_ONLY, "pair keeps next=Tap Only", pageTurnGesture, TAP_ONLY);
  expect(previousPageGesture == INVERTED_TAP, "pair keeps previous=Inverted Tap", previousPageGesture, INVERTED_TAP);

  // 4. Tap classification. Both directions Disabled: the tap branch is quiet on
  //    every third of the screen -- a value that silently folded back to Tap
  //    Only would turn here.
  for (int x : {50, 600, 1150}) {
    expect(tapAt(x, 400, PAGE_TURN_GESTURE_DISABLED, PAGE_TURN_GESTURE_DISABLED, false) == tapzones::NONE,
           "both Disabled: no tap turns", tapAt(x, 400, PAGE_TURN_GESTURE_DISABLED, PAGE_TURN_GESTURE_DISABLED, false),
           tapzones::NONE);
  }
  // Next=Tap Only, previous=Disabled: the legacy thirds rule makes the whole
  // surface turn forward when only one direction taps (nextTaps && !prevTaps),
  // and only that direction. Pinned here because "Disabled" must not silently
  // re-enable the other side.
  expect(tapAt(1100, 400, TAP_ONLY, PAGE_TURN_GESTURE_DISABLED, false) == tapzones::NEXT,
         "Tap Only turns the next side", tapAt(1100, 400, TAP_ONLY, PAGE_TURN_GESTURE_DISABLED, false),
         tapzones::NEXT);
  expect(tapAt(50, 400, TAP_ONLY, PAGE_TURN_GESTURE_DISABLED, false) == tapzones::NEXT,
         "Tap Only never turns backwards", tapAt(50, 400, TAP_ONLY, PAGE_TURN_GESTURE_DISABLED, false),
         tapzones::NEXT);
  expect(tapAt(1100, 400, PAGE_TURN_GESTURE_DISABLED, TAP_ONLY, false) == tapzones::PREV,
         "previous-only taps never turn forward", tapAt(1100, 400, PAGE_TURN_GESTURE_DISABLED, TAP_ONLY, false),
         tapzones::PREV);
  // Both directions Tap: both thirds turn.
  expect(tapAt(1100, 400, TAP_ONLY, TAP_ONLY, false) == tapzones::NEXT, "both Tap: next side turns",
         tapAt(1100, 400, TAP_ONLY, TAP_ONLY, false), tapzones::NEXT);
  expect(tapAt(50, 400, TAP_ONLY, TAP_ONLY, false) == tapzones::PREV, "both Tap: previous side turns",
         tapAt(50, 400, TAP_ONLY, TAP_ONLY, false), tapzones::PREV);
  // Inverted Tap swaps the sides; that is the gesture's whole meaning and it is
  // applied at read time, so the stored value never has to be rewritten.
  expect(tapAt(1100, 400, INVERTED_TAP, INVERTED_TAP, true) == tapzones::PREV,
         "Inverted Tap swaps the next side", tapAt(1100, 400, INVERTED_TAP, INVERTED_TAP, true), tapzones::PREV);
  // Swipe Only accepts no tap at all.
  expect(tapAt(1100, 400, SWIPE_ONLY, SWIPE_ONLY, false) == tapzones::NONE, "Swipe Only: no tap turns",
         tapAt(1100, 400, SWIPE_ONLY, SWIPE_ONLY, false), tapzones::NONE);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("PASS: page-turn gesture values, migration and tap classification\n");
  return 0;
}
'''


def main():
    source = (
        HARNESS.replace('__ENUM__', gesture_enum())
        .replace('__GESTURE_ALLOWS_TAP__', gesture_allows_tap())
        .replace('__MIGRATION__', migration_block())
    )
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / 'page_turn_gestures.cpp'
        binary = Path(tmp) / 'page_turn_gestures'
        cpp.write_text(source)
        subprocess.run(
            ['c++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
             str(cpp), '-o', str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
