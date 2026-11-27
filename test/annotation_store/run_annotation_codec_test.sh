#!/usr/bin/env bash
# Host regression for the annotation codec (see AnnotationCodecTest.cpp).
# Bare g++: the suite needs no ESP headers and no cmake FetchContent.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/annotation_codec_build"
CXX="${CXX:-g++}"
mkdir -p "$BUILD"

INCLUDES=(-I"$ROOT/lib/Epub/Epub")
SOURCES=("$ROOT/test/annotation_store/AnnotationCodecTest.cpp" "$ROOT/lib/Epub/Epub/AnnotationStore.cpp")

"$CXX" -std=c++20 -O1 -Wall -Wextra "${INCLUDES[@]}" -o "$BUILD/annotation_codec_test" "${SOURCES[@]}"
"$BUILD/annotation_codec_test"

# Mutation: drop the UTF-8 boundary backoff. Truncating mid-codepoint must be
# caught by the clamp test, or the suite is not actually watching that rule.
python3 - "$ROOT/lib/Epub/Epub/AnnotationStore.cpp" "$BUILD/AnnotationStore.mut.cpp" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
needle = "while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;"
assert needle in text, "mutation anchor missing"
open(dst, "w").write(text.replace(needle, "// mutation: no boundary backoff"))
PY
"$CXX" -std=c++20 -O1 "${INCLUDES[@]}" -o "$BUILD/annotation_store_mut" \
  "$ROOT/test/annotation_store/AnnotationCodecTest.cpp" "$BUILD/AnnotationStore.mut.cpp"
if "$BUILD/annotation_store_mut" >/dev/null 2>&1; then
  echo "annotation store: mutation NOT detected" >&2
  exit 1
fi
echo "== mutation detected"
