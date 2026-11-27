#!/usr/bin/env bash
# Host check for the publish-outcome rules (src/util/FilePublishRules.h).
#
# The rules decide what a caller tells the user when a downloaded file has to
# replace an existing one. Two mistakes shipped in 0.8.2: a byte-for-byte
# overwrite counted as a storage failure ("无法写入存储卡" after a complete font
# download), and every distinct FAT failure collapsed into one message, so the
# field report could not say which file was stuck. Each mutation re-creates one
# of those.
#
# The mutations run against a copy; the header is hashed before and after.
set -euo pipefail

cd "$(dirname "$0")/../.."

HEADER=src/util/FilePublishRules.h
TEST=test/file_publish/FilePublishRulesTest.cpp
OUT=${TMPDIR:-/tmp}/file_publish_host_test
MUTDIR=${TMPDIR:-/tmp}/file_publish_mut
BEFORE=$(shasum -a 256 "$HEADER" | awk '{print $1}')

g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I src -o "$OUT" "$TEST"
"$OUT"

# The test includes the header as util/FilePublishRules.h, so the mutated copy
# has to sit under the same relative path for -I to shadow the tree of record.
rm -rf "$MUTDIR"
mkdir -p "$MUTDIR/util"

mutate() {
  python3 - "$MUTDIR/util/FilePublishRules.h" "$1" "$2" <<'PY'
import sys
path, needle, replacement = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()
if src.count(needle) != 1:
    sys.exit("mutation needle missing or ambiguous (%d occurrences): %s" % (src.count(needle), needle))
open(path, 'w').write(src.replace(needle, replacement))
PY
}

run_mutation() {  # run_mutation <name> <needle> <replacement>
  local name=$1
  cp "$HEADER" "$MUTDIR/util/FilePublishRules.h"
  mutate "$2" "$3"
  if g++ -std=c++20 -O1 -Wall -Wextra -I "$MUTDIR" -I src \
       -o "${MUTDIR}/test" "$TEST" > /dev/null 2>&1; then
    if "${MUTDIR}/test" > /dev/null 2>&1; then
      echo "FAIL: the ${name} mutation was not detected"
      exit 1
    fi
  else
    echo "FAIL: the ${name} mutation did not compile"
    exit 1
  fi
  echo "PASS: ${name} mutation detected"
}

# 1. The missing source stops mattering: publishing nothing reports success.
run_mutation "missing source ignored" \
  '  if (!a.sourcePresent) return Outcome::SourceMissing;' \
  '  if (false) return Outcome::SourceMissing;'

# 2. The 0.8.2 message bug: a working copy-over is not treated as success.
run_mutation "copy-over not a success" \
  '  if (a.copyOk) return Outcome::CopiedOver;' \
  '  if (false) return Outcome::CopiedOver;'

# 3. A replace mislabelled as a plain rename: the two must stay distinguishable
#    because only one of them deleted a file the user already had.
run_mutation "replace mislabelled as rename" \
  'return a.destPresent ? Outcome::Replaced : Outcome::Renamed;' \
  'return Outcome::Renamed;'

# 4. A stuck destination and a plain copy failure given the same label.
run_mutation "stuck destination collapsed" \
  'return a.destPresent ? Outcome::DestinationStuck : Outcome::CopyFailed;' \
  'return Outcome::CopyFailed;'

# 5. succeeded() widened to cover a stuck destination.
run_mutation "stuck counts as success" \
  'return outcome == Outcome::Renamed || outcome == Outcome::Replaced || outcome == Outcome::CopiedOver;
' \
  'return outcome == Outcome::Renamed || outcome == Outcome::Replaced || outcome == Outcome::CopiedOver ||
         outcome == Outcome::DestinationStuck;
'

# 6. Two outcomes sharing one log label (the switch copy-paste mistake).
run_mutation "labels collide" \
  '    case Outcome::CopiedOver:
      return "copied-over";' \
  '    case Outcome::CopiedOver:
      return "replaced";'

AFTER=$(shasum -a 256 "$HEADER" | awk '{print $1}')
if [ "$BEFORE" != "$AFTER" ]; then
  echo "FAIL: the original header changed (${BEFORE} -> ${AFTER})"
  exit 1
fi
echo "PASS: ${HEADER} untouched"
echo "file_publish: all mutations detected"
