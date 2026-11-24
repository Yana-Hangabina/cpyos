#!/bin/bash
# Overlay this project's MuPDF changes onto a fetched 1.26.3 source tree and
# trim the built-in base-14 fonts.  Idempotent; safe before incremental builds.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S="${1:-$HERE/mupdf-1.26.3-source}"

if [ ! -d "$S" ]; then
  echo "missing MuPDF source at $S" >&2
  echo "fetch https://mupdf.com/downloads/archive/mupdf-1.26.3-source.tar.gz and extract it there" >&2
  exit 1
fi

cp -R "$HERE/source/." "$S/source/"
cp -R "$HERE/include/." "$S/include/"
cp "$HERE/timegm_compat.h" "$S/timegm_compat.h"

python3 - "$S/Makefile" <<'PY_MAKEFILE'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
needle = "# Read Pico PDF module: built-in base-14 fonts are not shipped (TOFU_BASE14)."
if needle not in text:
    anchor = "FONT_BIN := $(sort $(wildcard resources/fonts/urw/*.cff))\n"
    if anchor not in text:
        raise SystemExit("FONT_BIN anchor missing in " + str(path))
    text = text.replace(
        anchor,
        anchor
        + needle
        + "\nFONT_BIN := $(filter-out resources/fonts/urw/%.cff, $(FONT_BIN))\n",
        1,
    )
    path.write_text(text)
PY_MAKEFILE

echo "prepared $S"
