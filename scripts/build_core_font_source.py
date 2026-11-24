#!/usr/bin/env python3
"""Regenerate assets/fonts/CPYOSHanSansSC-Core.ttf (tier-1 reading core source).

The core face is the notdef-free subset used for book text: the 3,500 most common
hanzi plus ASCII and the common punctuation the UI uses. It is deliberately smaller than the
8,105-character family: the XIP layout's data partition leaves 768 KiB for the
compressed core carrier, and this subset compresses to ~0.7 MiB.

Run only when the 8,105 source or the character contract changes; the binary is
committed so the firmware build stays deterministic without fontTools.
"""
import pathlib

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Regular-8105.ttf"
OUT = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Core.ttf"

# The 3,500 most common characters (the same list the built-in bitmap reader
# uses). Level-1 minus its rare tail still compresses past the XIP partition
# budget; this list keeps the coverage that matters for running text.
chars = set((ROOT / "lib" / "EpdFont" / "scripts" / "cn_common_chars.txt")
            .read_text(encoding="utf-8").strip())
# UI-only codepoints stay on the app0 UI face; adding them here would grow the
# carrier past the data-partition budget with no benefit to book text.
chars.update(chr(c) for c in range(0x20, 0x7F))
chars.update("·—…「」『』《》〈〉（）【】、。，；：？！“”‘’％")

options = subset.Options()
options.hinting = False
options.desubroutinize = True
options.layout_features = []
options.name_IDs = ["*"]
options.notdef_outline = True

font = TTFont(SRC)
subsetter = subset.Subsetter(options=options)
subsetter.populate(text="".join(sorted(chars)))
subsetter.subset(font)
font.save(OUT)
print(f"{OUT}: {OUT.stat().st_size} B, {len(chars)} chars")
