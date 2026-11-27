#!/usr/bin/env bash
# Host check for the memory contract table (lib/Memory/MemoryContracts.h).
#
# Mutations are the ways a table like this stops meaning anything: an unpaired
# release accepted silently, a limit that does not bound its own ask, a yield
# order that ignores releasability, a cap of zero read as "fits anything".
set -euo pipefail

cd "$(dirname "$0")/../.."

HEADER=lib/Memory/MemoryContracts.h
TEST=test/memory_contracts/MemoryContractsTest.cpp
OUT=${TMPDIR:-/tmp}/memory_contracts_host_test
MUTDIR=${TMPDIR:-/tmp}/memory_contracts_mut
BEFORE=$(shasum -a 256 "$HEADER" | awk '{print $1}')

g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I lib/Memory -o "$OUT" "$TEST"
"$OUT"

rm -rf "$MUTDIR"
mkdir -p "$MUTDIR"

mutate() {
  python3 - "$MUTDIR/MemoryContracts.h" "$1" "$2" <<'PY'
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
  cp "$HEADER" "$MUTDIR/MemoryContracts.h"
  mutate "$2" "$3"
  if g++ -std=c++20 -O1 -Wall -Wextra -I "$MUTDIR" -o "${MUTDIR}/test" "$TEST" > /dev/null 2>&1; then
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

# 1. The 0.8.2 defect accepted as normal: a feature released for the radio with
#    nothing that puts it back.
run_mutation "unpaired release allowed" \
  '  return entry.releases != Release::ForNetwork || entry.restorer != Restorer::None;' \
  '  return true;'

# 2. A limit that does not bound the ask it is quoted against.
run_mutation "limit below its own ask" \
  '    {"font.readerFace", 1024u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},' \
  '    {"font.readerFace", 128u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},'

# 3. Yield order ignoring releasability: the reclaim would then drop features that
#    cannot be restored in order to free room it does not need.
run_mutation "yield order ignores release" \
  '      if (taken || entries[i].releases != Release::ForNetwork) continue;' \
  '      if (taken) continue;'

# 4. An unmeasured cap of zero read as "anything fits".
run_mutation "no cap means pass" \
  '  if (entry.declaredLimitBytes == 0) return false;' \
  '  if (entry.declaredLimitBytes == 0) return true;'

# 5. A restorer claimed for a feature that is never released: the pairing
#    statement stops describing any real path.
run_mutation "phantom restorer allowed" \
  '    {"font.emoji", 844596u + 96u * 1024u, 844596u, Release::Never, Restorer::None, true},' \
  '    {"font.emoji", 844596u + 96u * 1024u, 844596u, Release::Never, Restorer::RenderMemory, true},'

# 6. The §20 row losing its pairing while the table still reads as complete.
run_mutation "font pairing forgotten" \
  '    {"font.readerFace", 1024u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},' \
  '    {"font.readerFace", 1024u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::SelfLimited, true},'

AFTER=$(shasum -a 256 "$HEADER" | awk '{print $1}')
if [ "$BEFORE" != "$AFTER" ]; then
  echo "FAIL: the original header changed (${BEFORE} -> ${AFTER})"
  exit 1
fi
echo "PASS: ${HEADER} untouched"
echo "memory_contracts: all mutations detected"
