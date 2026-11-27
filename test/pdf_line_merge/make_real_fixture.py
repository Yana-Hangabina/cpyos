#!/usr/bin/env python3
"""Regenerate the real-text fixtures embedded in PdfLineMergeTest.cpp.

The merge rules were first tested against strings I wrote, which can only contain the
shapes I thought of.  This pulls the three arrays straight out of a PDF that ships in
this tree -- a genuine table of contents and a genuine page of body text -- using
MuPDF, i.e. the same engine family the `pdfmod` module is built from:

    python3 test/pdf_line_merge/make_real_fixture.py > /tmp/fixture.txt
    diff /tmp/fixture.txt <(sed -n '/kRealTocRows/,/^}/p' test/pdf_line_merge/PdfLineMergeTest.cpp)

Expect the arrays to match.  If they do not, MuPDF's version moved and the embedded
fixture -- together with the 34 -> 34 / 24 -> 15 / 10 -> 6 numbers documented in
docs/engineering/pdf-xip.md -- needs re-measuring, not just copying.
"""
from __future__ import annotations

import pathlib
import sys

try:
    import fitz  # PyMuPDF
except ImportError:  # pragma: no cover - host-side tooling only
    raise SystemExit("needs PyMuPDF (python3 -m pip install pymupdf); host-side only")

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOK = ROOT / "patches/mupdf/mupdf-1.26.3-source/thirdparty/lcms2/doc/LittleCMS2.16 API.pdf"
TOC_PAGE = 2
PROSE_PAGE = 20


def c_array(name: str, rows: list[str]) -> str:
    out = [f"static const char* {name}[] = {{"]
    for row in rows:
        text = row.strip()
        if not text:
            continue
        out.append('      "' + text.replace("\\", "\\\\").replace('"', '\\"') + '",')
    out.append("};")
    return "\n".join(out)


def main() -> int:
    if not BOOK.exists():
        raise SystemExit(f"fixture source not found: {BOOK}")
    doc = fitz.open(BOOK)
    print(f"// source: {BOOK.relative_to(ROOT)}")
    # Printed verbatim because the extraction is version-sensitive: the embedded fixture
    # came from a different MuPDF minor version than this host has, and regeneration must
    # say which one produced what it just wrote.
    print("// extracted with " + (fitz.__doc__ or "PyMuPDF " + str(fitz.version)).strip().replace(chr(10), " / "))
    print(c_array("kRealTocRows", [l for l in doc[TOC_PAGE].get_text("text").splitlines() if l.strip()][:34]))
    print()
    print(c_array("kRealProseLines", [l for l in doc[PROSE_PAGE].get_text("text").splitlines() if l.strip()][:24]))
    print()
    # The test inlines this list (it is short), so the name here is only for the diff.
    print(c_array("kRealProseBlocks",
                  [b[4].strip().replace("\n", " ") for b in doc[PROSE_PAGE].get_text("blocks") if b[4].strip()][:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
