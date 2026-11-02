#!/usr/bin/env bash
# Host check for the pagination LUT container (Section.h, upstream 2.0.0 layout).
#
# Compiles the real lib/Epub/Epub/Section.h against the parser test's HAL stubs: no cmake,
# no firmware toolchain, no Section.cpp link (only the nested BuildContext type is named).
# Also runs a mutation step: reverting the container to std::vector must fail the static
# assertion, which proves the pin is load-bearing.
set -euo pipefail

cd "$(dirname "$0")/../.."

STUBS=test/chapter_html_slim_parser/stubs
INCLUDES=(-I "$STUBS" -I lib -I lib/Epub -I lib/Epub/Epub -I lib/Epub/Epub/parsers -I lib/Epub/Epub/css
          -I lib/EpdFont -I lib/Utf8 -I lib/Memory -I lib/Serialization -I lib/MiniBidi -I lib/FsHelpers
          -I lib/XmlParserUtils)
OUT=${TMPDIR:-/tmp}/section_lut_host_test

g++ -std=c++20 -O1 -Wall -Wextra "${INCLUDES[@]}" -o "$OUT" test/section_lut/LutContainerTest.cpp
"$OUT"

# Mutation: put the vector back. The static_assert must reject it.
MUT=${TMPDIR:-/tmp}/section_lut_mut
rm -rf "$MUT"
mkdir -p "$MUT/Epub"
cp lib/Epub/Epub/Section.h "$MUT/Epub/Section.h"
python3 - "$MUT/Epub/Section.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "std::deque<PageLutEntry> lut;"
if needle not in src:
    sys.exit("mutation needle missing: the LUT container declaration changed shape")
open(path, "w").write(src.replace(needle, "std::vector<PageLutEntry> lut;"))
PY
if g++ -std=c++20 -O1 -I "$MUT" "${INCLUDES[@]}" -o "${MUT}/test" test/section_lut/LutContainerTest.cpp > /dev/null 2>&1; then
  echo "FAIL: reverting the LUT container to std::vector did not fail the test"
  exit 1
fi
echo "PASS: vector mutation detected"
