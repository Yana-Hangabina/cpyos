#!/usr/bin/env bash
# Host check for the installed-core rule (src/SystemCoreFontCheck.h).
#
# The rule answers two questions that were both wrong on shipped firmware: does this
# device have a usable reading core, and may the installer overwrite the file on the
# card? An unchecked marker write made a good face read as absent (tofu for characters
# the partition carries); a size-only comparison made the installer delete the user's
# downloaded 8,105 pack. The test enumerates all 128 fact combinations, and every
# mutation below re-creates one of those mistakes to prove the pin is load-bearing.
#
# The mutations run against a copy. The original header is hashed before and after so
# a broken patch can never leave a marker behind in the tree of record.
set -euo pipefail

cd "$(dirname "$0")/../.."

HEADER=src/SystemCoreFontCheck.h
OUT=${TMPDIR:-/tmp}/system_core_font_check_host_test
MUTDIR=${TMPDIR:-/tmp}/system_core_font_check_mut
BEFORE=$(shasum -a 256 "$HEADER" | awk '{print $1}')

g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I src \
    -o "$OUT" test/system_core_font_check/CoreCheckTest.cpp
"$OUT"

rm -rf "$MUTDIR"
mkdir -p "$MUTDIR"

mutate() {  # mutate.py <needle> <replacement>
  python3 - "$MUTDIR/SystemCoreFontCheck.h" "$1" "$2" <<'PY'
import sys
path, needle, replacement = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()
if src.count(needle) != 1:
    sys.exit("mutation needle missing or ambiguous (%d occurrences): %s" % (src.count(needle), needle))
open(path, 'w').write(src.replace(needle, replacement))
PY
}

run_mutation() {  # run_mutation <name>
  local name=$1
  cp "$HEADER" "$MUTDIR/SystemCoreFontCheck.h"
  shift
  mutate "$1" "$2"
  if g++ -std=c++20 -O1 -Wall -Wextra -I "$MUTDIR" \
       -o "${MUTDIR}/test" test/system_core_font_check/CoreCheckTest.cpp > /dev/null 2>&1; then
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

# 1. The original bug shape: a matching size is treated as proof the face is ours,
#    so a downloaded 8,105 pack gets certified and then overwritten.
run_mutation "size-only trust" \
  '  if (!facts.sizeMatches) return Verdict::ForeignSize;' \
  '  if (!facts.sizeMatches) return Verdict::NotInstalled;'

# 2. Same-length foreign content certified from the size alone.
run_mutation "same-size foreign content" \
  '  if (!facts.contentMatches) return Verdict::ForeignContent;' \
  '  if (!facts.contentMatches && false) return Verdict::ForeignContent;'

# 3. The installer allowed to rewrite a face it just identified as foreign.
run_mutation "overwrite a foreign face" \
  '  return verdict == Verdict::NotInstalled || verdict == Verdict::Unreadable;' \
  '  return verdict == Verdict::NotInstalled || verdict == Verdict::Unreadable ||
         verdict == Verdict::ForeignSize;'

# 4. Repairing the marker without checking the bytes.
run_mutation "repair without verification" \
  '  if (!facts.contentReadable) return Verdict::Unreadable;' \
  '  if (!facts.contentReadable) return Verdict::RepairMarker;'

# 5. The tier label checked against its own NUL padding instead of only its label bytes.
#    This was the first implementation, and it rejected every real carrier: a sweep over
#    all 256 first-byte values is what caught it.
run_mutation "label padding treated as label text" \
  '    if (i >= length) {
      if (field[i] != 0) return out;
      continue;
    }' \
  '    if (i >= length && field[i] != 0) return out;'

# 6. Accepting any bytes as a label, which would print a tier that the blob does not carry.
run_mutation "unvalidated tier label accepted" \
  '    if (!isTierChar(static_cast<char>(field[i]))) return out;' \
  '    if (false) return out;'

# 7. The label must stay reporting-only. tierLabel() is read by logs and by Status; if it
#    ever reaches a condition, a carrier written before the field existed -- or a blob with
#    a corrupt label -- would turn a working font into "no core", the failure this whole
#    header exists to prevent. Statement-aware, because the real calls wrap across lines: a
#    per-line grep flagged two log continuations as decision code.
python3 - src/SystemCoreFont.cpp <<'PY'
import sys


def mask(text):
    """Blank comments and shorten nothing: replace literal bodies with X, same length.

    Without this, the first version of this gate flagged two legitimate log calls, because
    the format string contained a semicolon and that looked like the end of the statement.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text[i:i + 2] == '//':
            while i < n and text[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and text[i:i + 2] == '/*':
            i += 2
            while i < n and text[i:i + 2] != '*/':
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            out[i - 1] = out[i] = ' '
            i += 1
        elif c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    out[i] = ' '
                    i += 1
                if i < n and text[i] != '\n':
                    out[i] = 'X'
                i += 1
            i += 1
        else:
            i += 1
    return ''.join(out)


raw = open(sys.argv[1]).read()
text = mask(raw)
# Reporting uses allowed here. A declaration of the accessor itself is matched by the
# "ends with const char*" case, so an unrelated `const char* x = tierLabel()` is still
# flagged; a decision hidden in the same statement as a LOG_ call is not, which is the
# known limit of a source-text gate.
offenders = []
index = 0
while True:
    index = text.find('tierLabel(', index)
    if index < 0:
        break
    boundary = max(text.rfind(';', 0, index), text.rfind('{', 0, index), text.rfind('}', 0, index))
    head = ' '.join(text[boundary + 1:index].split())
    if 'LOG_' not in head and 'out.tier =' not in head and not head.endswith('const char*'):
        offenders.append((raw.count('\n', 0, index) + 1, head[-70:]))
    index += len('tierLabel(')
if offenders:
    print('FAIL: the core decision path reads the tier label (reporting only):')
    for line, context in offenders:
        print(f'  src/SystemCoreFont.cpp:{line}: ...{context}')
    sys.exit(1)
print('PASS: tierLabel() is used for reporting only')
PY

AFTER=$(shasum -a 256 "$HEADER" | awk '{print $1}')
if [ "$BEFORE" != "$AFTER" ]; then
  echo "FAIL: the mutation harness modified $HEADER"
  exit 1
fi
echo "PASS: $HEADER untouched by the harness (sha256 $BEFORE)"
