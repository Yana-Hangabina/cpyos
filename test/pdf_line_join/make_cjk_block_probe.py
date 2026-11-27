#!/usr/bin/env python3
"""Reproduce the measurement that motivated src/pdf/PdfLineJoin.h.

Two Chinese paragraphs, each hard-wrapped at 16 characters, laid out on one uniform
16 pt pitch with no blank line and no indent -- the shape a lot of OCR'd Chinese PDFs
have.  The question is what MuPDF hands the extractor, because the answer decides
where a paragraph break can still be recovered:

  * if MuPDF returns one block per paragraph, the app-side `pdf_merge` rule can work;
  * if MuPDF returns ONE block with all six lines, the old unconditional glue in
    `PdfDocument::extractText()` ran the two paragraphs together into a single string
    and no app-side rule could ever split them again -- the newline is gone by then.

Measured with this script (host PyMuPDF 1.28.0 / MuPDF 1.29.0; the device module builds
1.26.3, so treat the block count as the shape it demonstrates, not a byte-exact device
dump -- the device-side check is `exp/pdf_verify_matrix.py --sweep N`):

    1 block, 6 lines;  old glue -> 1 paragraph;  shipped rule -> 2 paragraphs

The paragraph count comes from PdfLineJoinProbe.cpp, i.e. from `paragraphStarts()` in
src/pdf/PdfLineJoin.h -- the same function the module calls.  An earlier version of this
script carried its own copy of the rule ("keep in step with the header"), which quietly
went stale the day the header grew the text-column gate: the numbers stayed plausible
while no longer describing the shipped code.  There is now one implementation to read.

    python3 test/pdf_line_join/make_cjk_block_probe.py
"""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

try:
    import fitz
except ImportError:
    raise SystemExit("needs PyMuPDF (python3 -m pip install pymupdf); host-side probe only")

import join_real_pages as measure  # noqa: E402  (shares the probe driver and the labelling)

FIRST = "本书如何帮助你成为更好的读者，我们需要从最基础的部分讲起，然后再逐步深入"
SECOND = "更复杂的技巧与结构。第二章讨论排版本身，包括行距、字距与段落首行缩进的处理"
PER_LINE = 16
PITCH = 16.0
LEFT, SIZE = 30.0, 12.0


def wrap(text: str) -> list[str]:
    return [text[i:i + PER_LINE] for i in range(0, len(text), PER_LINE)]


def main() -> int:
    measure.build_probe()
    doc = fitz.open()
    page = doc.new_page(width=400, height=600)
    y = 60.0
    for line in wrap(FIRST) + wrap(SECOND):
        page.insert_text((LEFT, y), line, fontname="china-s", fontsize=SIZE)
        y += PITCH
    doc.save("/tmp/cpyos_cjk_two_paragraphs.pdf")
    doc.close()

    probe = fitz.open("/tmp/cpyos_cjk_two_paragraphs.pdf")
    blocks = measure.collect_blocks(probe[0])
    probe.close()
    rows = measure.run_probe(blocks)
    print(f"MuPDF blocks: {len(blocks)} ({[len(b) for b in blocks]} lines)")
    bad = 0
    for index, (lines, (starts, stats)) in enumerate(zip(blocks, rows)):
        print(f"  block {index}: {len(lines)} lines, {stats[0]} printable, {stats[1]} reaching "
              f"the margin -> old glue 1 paragraph; shipped rule {len(starts)}")
        if index == 0 and len(blocks) == 1 and len(starts) != 2:
            print("  FAIL: the single-block case must recover exactly two paragraphs",
                  file=sys.stderr)
            bad += 1
    if len(blocks) != 1:
        print("note: MuPDF grouped these paragraphs already; the premise changed (still fine,"
              " the app-side rule covers it)", file=sys.stderr)
    return bad


if __name__ == "__main__":
    sys.exit(main())
