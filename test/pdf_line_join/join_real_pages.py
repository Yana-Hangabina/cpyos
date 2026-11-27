#!/usr/bin/env python3
"""Measure the SHIPPED pdf_join walk on real pages, and print every split it makes.

The point of this script is that it contains no copy of the rule.  It reads line geometry
out of a real PDF, hands it to PdfLineJoinProbe.cpp -- which includes src/pdf/PdfLineJoin.h
and calls the same `paragraphStarts()` the module calls -- and reports what came back.  A
Python restatement would have been a second implementation free to drift from the first,
which is the mistake this feature has already made twice.

    python3 test/pdf_line_join/join_real_pages.py                 # default pages
    python3 test/pdf_line_join/join_real_pages.py 20 21 22        # specific pages

Numbers this produced are quoted in docs/engineering/pdf-xip.md; re-run it after changing
any threshold, and re-check the CJK case with make_cjk_block_probe.py.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys

try:
    import fitz  # PyMuPDF
except ImportError:  # pragma: no cover - host-side tooling only
    raise SystemExit("needs PyMuPDF (python3 -m pip install pymupdf); host-side only")

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOK = ROOT / "patches/mupdf/mupdf-1.26.3-source/thirdparty/lcms2/doc/LittleCMS2.16 API.pdf"
PROBE = pathlib.Path("/tmp/pdf_line_join_probe")
BUILD = ROOT / "test/pdf_line_join/PdfLineJoinProbe.cpp"
HEADER_DIR = ROOT / "src/pdf"
DEFAULT_PAGES = (2, 5, 20, 30)

# Mirrors pdf_join::isBlankCp, and is only used to LABEL a line for the probe: the decision
# is made in C++.  If the two ever disagree, run_pdf_line_join_test.sh's blank-set checks and
# PdfLineJoinTest are the place that will show it.
def is_blank_cp(cp: int) -> bool:
    return (
        cp <= 0x20
        or cp in (0xA0, 0xAD, 0x202F, 0x205F, 0x2060, 0xFEFF, 0x3000, 0x3164)
        or 0x2000 <= cp <= 0x200F
    )


def line_is_printable(text: str) -> bool:
    return any(not is_blank_cp(ord(ch)) for ch in text)


def build_probe() -> None:
    subprocess.run(
        ["g++", "-std=c++20", "-O1", "-Wall", "-Wextra", f"-I{HEADER_DIR}", "-o", str(PROBE), str(BUILD)],
        check=True,
    )


def collect_blocks(page) -> list[list[tuple[str, float, float, float, float]]]:
    blocks = []
    for block in page.get_text("dict")["blocks"]:
        if block.get("type") != 0:
            continue
        lines = []
        for line in block.get("lines", []):
            text = "".join(span.get("text", "") for span in line.get("spans", []))
            x0, y0, x1, y1 = line["bbox"]
            lines.append((text, x0, y0, x1, y1))
        if lines:
            blocks.append(lines)
    return blocks


def run_probe(blocks) -> list[tuple[list[int], tuple[int, int]]]:
    stdin = []
    for lines in blocks:
        stdin.append(f"BLOCK {len(lines)}")
        for text, x0, y0, x1, y1 in lines:
            stdin.append(f"{1 if line_is_printable(text) else 0} {x0} {y0} {x1} {y1}")
    done = subprocess.run([str(PROBE)], input="\n".join(stdin) + "\n",
                          capture_output=True, text=True, check=True)
    rows = done.stdout.split("\n")
    out = []
    index = 0
    while index + 1 < len(rows):
        starts = [int(token) for token in rows[index].split()[2:]]
        start_count = int(rows[index].split()[1])
        assert len(starts) == start_count, "probe output truncated"
        stats = tuple(int(token) for token in rows[index + 1].split()[1:])
        out.append((starts, stats))
        index += 2
    return out


def main(argv) -> int:
    pages = tuple(int(arg) for arg in argv if arg.isdigit()) or DEFAULT_PAGES
    if not BOOK.exists():
        print(f"missing fixture PDF: {BOOK}", file=sys.stderr)
        return 2
    # Always rebuild: it costs about a second, and a stale probe would silently measure the
    # previous version of the rule.  PDF_JOIN_PROBE overrides both, which is how the
    # "what would the naive rule have done" number below was produced -- against a probe
    # built from a mutated copy of the header, not against a second implementation.
    override = os.environ.get("PDF_JOIN_PROBE")
    if override:
        globals()["PROBE"] = pathlib.Path(override)
    else:
        build_probe()
    doc = fitz.open(BOOK)
    total_blocks = total_paras = total_splits = 0
    for number in pages:
        blocks = collect_blocks(doc[number - 1])
        rows = run_probe(blocks)
        assert len(rows) == len(blocks)
        # The baseline is NOT one paragraph per block: a block whose every line is blank
        # contributes no text at all, because the extractor pushes a paragraph only when it
        # has characters.  Counting those blocks as paragraphs made every page look like it
        # was LOSEING splits, which is how this script's first numbers disagreed with the
        # split lines it printed underneath them.
        baseline = sum(1 for lines in blocks if any(line_is_printable(t) for t, *_ in lines))
        paras = sum(len(starts) for starts, _ in rows)
        splits = paras - baseline
        total_splits += splits
        total_blocks += baseline
        total_paras += paras
        print(f"page {number}: text blocks={baseline} (of {len(blocks)})  "
              f"shipped rule={paras}  (+{splits})")
        for lines, (starts, _stats) in zip(blocks, rows):
            for position in range(1, len(starts)):
                after = lines[starts[position]][0]
                # The line before a start is often the blank that caused the break, which
                # prints as '' and tells nothing: walk back to the last line with ink.
                back = starts[position] - 1
                while back >= 0 and not line_is_printable(lines[back][0]):
                    back -= 1
                before = lines[back][0] if back >= 0 else ""
                marker = " [after a blank line]" if back != starts[position] - 1 else ""
                print(f"    split at line {starts[position]}{marker}: \u2026{before.strip()[-38:]!r} | "
                      f"{after.strip()[:38]!r}")
    doc.close()
    print(f"TOTAL over {len(pages)} pages: {total_blocks} text blocks -> {total_paras} "
          f"paragraphs ({total_splits} splits)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
