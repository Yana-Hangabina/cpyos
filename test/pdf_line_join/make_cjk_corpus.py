#!/usr/bin/env python3
"""CJK census: how the shipped block-splitting rule behaves on Chinese text at scale.

The rule exists because MuPDF fuses hard-wrapped Chinese paragraphs into ONE text block
(make_cjk_block_probe.py measures the shape).  The refinement that gated the ragged test
was validated on a Latin manual; this script is the Chinese half of that evidence, with
GROUND TRUTH -- every paragraph count below is a number this script chose, not a number it
inferred from the output, so a rule that splits too much or too little shows up as a
difference rather than as an impression.

Six shapes, deliberately spanning the rule's decision surface:

  wrapped3     two paragraphs of three lines each, uniform pitch, no blank  -> must recover 2
  wrapped4     two paragraphs of four lines each                            -> must recover 2
  short-pair   two paragraphs of TWO lines each (the known miss)            -> recovers 1
  blank-split two paragraphs separated by a blank line inside one block     -> must recover 2
  listing      one indented, ragged-everywhere block (a code listing)       -> must stay 1
  justified    two paragraphs of three fully right-reaching lines           -> recovers 1
               (the ragged test sees nothing; only the blank-line rule can help, so this
                row documents where the geometry alone is blind)

Run:  python3 test/pdf_line_join/make_cjk_corpus.py [pages]
Exit code is non-zero when a MUST row misses, so this is a check, not a report.
"""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

try:
    import fitz
except ImportError:
    raise SystemExit("needs PyMuPDF (python3 -m pip install pymupdf); host-side tooling only")

import join_real_pages as measure  # noqa: E402  (shares the probe driver)

OUT = pathlib.Path("/tmp/cpyos_cjk_corpus.pdf")
PITCH = 16.0
SIZE = 12.0
LEFT = 30.0
WRAP = 16  # characters per visual line

# A pool of Chinese characters with no punctuation, so a line's measured width depends only
# on how many characters it holds.  Punctuation would add narrow glyphs and blur the column.
POOL = "文学城市阅读字体设计排版行距字距段落缩进处理技巧结构内容页面显示墨水屏幕电子纸书架笔记标注搜索"


def text_lines(count: int, seed: int, tail: int = WRAP,
               widths: list[int] | None = None) -> list[str]:
    """`count` visual lines; the LAST one is `tail` characters long.

    Real wrapped prose ends its paragraphs mid-line, and for CJK that short final line IS
    the paragraph signal -- an earlier draft of this script filled every line to the column
    width and then reported the rule as failing on a shape that only exists in fixtures.
    `tail=WRAP` therefore models justified text, which the geometry genuinely cannot read
    (see the `uniform` case).
    """
    out = []
    for index in range(count):
        start = (seed * 7) % len(POOL)
        if widths is not None:
            width = widths[index]
        else:
            width = tail if index == count - 1 else WRAP
        out.append("".join(POOL[(start + i * 3) % len(POOL)] for i in range(width)))
        seed += 1
    return out


def build(cases) -> None:
    doc = fitz.open()
    page = doc.new_page(width=400, height=1200)
    y = 40.0
    for case in cases:
        for paragraph in case["paragraphs"]:
            widths = None
            if paragraph.get("vary"):
                # Every line a different length: a code listing, an index, a stanza.  This is
                # the shape the column gate exists to refuse -- measured on a real manual,
                # the same rule without the gate produced 28 splits where 7 were correct.
                widths = [4 + ((case["seed"] * 5 + i * 7) % (WRAP - 4))
                          for i in range(paragraph["lines"])]
            for line in text_lines(paragraph["lines"], case["seed"] + id(paragraph) % 11,
                                   paragraph.get("tail", 11), widths):
                x = LEFT + paragraph.get("indent", 0.0)
                width = WRAP - (1 if paragraph.get("indent") else 0)
                page.insert_text((x, y), line[:width], fontname="china-s", fontsize=SIZE)
                y += PITCH
            if paragraph.get("blank_after"):
                # A whitespace-only line: invisible on paper, and the reason the blank-line
                # rule exists.
                page.insert_text((LEFT, y), " ", fontname="china-s", fontsize=SIZE)
                y += PITCH
        y += 24.0  # air between CASES, so each case is its own MuPDF block
    doc.save(str(OUT))
    doc.close()


CASES = [
    {"name": "wrapped3", "want": 2, "hard": True, "seed": 1,
     "paragraphs": [{"lines": 3}, {"lines": 3}]},
    {"name": "wrapped4", "want": 2, "hard": True, "seed": 5,
     "paragraphs": [{"lines": 4}, {"lines": 4}]},
    {"name": "short-pair", "want": 1, "hard": False, "seed": 9,
     "paragraphs": [{"lines": 2}, {"lines": 2}]},
    {"name": "blank-split", "want": 2, "hard": True, "seed": 13,
     "paragraphs": [{"lines": 2, "blank_after": True}, {"lines": 2}]},
    {"name": "listing", "want": 1, "hard": True, "seed": 17,
     "paragraphs": [{"lines": 7, "vary": True}]},
    {"name": "justified", "want": 1, "hard": False, "seed": 21,
     "paragraphs": [{"lines": 3, "tail": WRAP}, {"lines": 3, "tail": WRAP}]},
    # Every line of every paragraph lands exactly on the margin: nothing is short, so the
    # ragged test has no signal and the pair stays fused.  Recorded as a blind spot with a
    # real number against it, not as an aspiration -- the only fix would be a gap or a blank
    # line, and justified CJK with no inter-paragraph space has neither.
    {"name": "uniform", "want": 1, "hard": False, "seed": 25,
     "paragraphs": [{"lines": 4, "tail": WRAP}, {"lines": 4, "tail": WRAP}]},
]


def main() -> int:
    measure.build_probe()
    build(CASES)
    doc = fitz.open(str(OUT))
    print(f"{'case':<12} {'mupdf blocks':>12} {'lines':>6} {'full':>5} {'rule':>5} "
          f"{'want':>5}  verdict")
    bad = 0
    for case in CASES:
        blocks = measure.collect_blocks(doc[0])
        # cases were separated by air, so the i-th block is the i-th case
        index = CASES.index(case)
        if index >= len(blocks):
            print(f"{case['name']:<12} MISSING -- MuPDF merged this case with its neighbour; "
                  "the corpus layout needs more air")
            bad += 1
            continue
        lines = blocks[index]
        starts_all, stats = measure.run_probe([lines])[0]
        verdict = ""
        if case["hard"] and len(starts_all) != case["want"]:
            verdict = "FAIL"
            bad += 1
        elif not case["hard"] and len(starts_all) != case["want"]:
            verdict = "note (differs from the documented expectation)"
        else:
            verdict = "ok" + ("" if case["hard"] else " (known miss reproduced)")
        print(f"{case['name']:<12} {1:>12} {len(lines):>6} {stats[1]:>5} "
              f"{len(starts_all):>5} {case['want']:>5}  {verdict}")
    doc.close()
    return bad


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
