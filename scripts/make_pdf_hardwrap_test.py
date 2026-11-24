#!/usr/bin/env python3
"""Make a hard-wrapped "OCR-shaped" PDF fixture for the reflow line merge.

Why this exists: `pdf_merge` is unit-tested against text vectors, but its rules
only mean something if a real extractor delivers real OCR pages the way the
tests assume. OCR text layers expose **every visual line as its own block** (one
`BT ... ET` per line) and mark a blank line with a whitespace-only line, so that
is exactly what this generator emits -- one text object per line, single column,
no images.

The expected result is fixed by the fixture, so an on-device run is a checkable
assertion instead of an impression. With the option ON the device logs
`reflow merge: N blocks -> M paragraphs`, and these eight text objects must
collapse to four paragraphs:

    1  The reflow engine must join these three        \
    2  counterpro-                                     } joined + de-hyphenated
    3  ductive lines into a single paragraph.         /
    4  <space only>                                   -> forces a paragraph break
    5  A short heading                                -> its own paragraph
    6     An indented line starts its own paragraph.   -> leading spaces
    7  It continues here because the previous line     \
    8  did not end a sentence at all                   } joined

With the option OFF the same page must keep eight separate paragraphs -- that
half of the check is what makes the toggle honest rather than decorative.

* The font is **embedded**, deliberately. This engine build has no MuPDF
  built-in fonts (space in `pdfmod` is tighter than space in the app), so a PDF
  that names a base-14 font without a `FontFile` renders as a completely blank
  page and yields no text at all. A base-14 fixture would fail at font
  resolution and look like a broken line merge. See
  docs/engineering/pdf-xip.md ("Non-embedded (base-14) fonts render blank").

Usage (the output is a throwaway test file; it is not shipped or committed):

  python3 scripts/make_pdf_hardwrap_test.py /tmp/pdf_hardwrap.pdf

Then put it on the card over the **network** (Settings > File transfer, the web
/upload form), because it is ~11 KB and `CMD:PUTFILE` over USB CDC cannot carry
that: measured 2026-11-19, 177 chunks of 64 bytes desynced after 1,920 bytes
with no PUTFILE_DONE, and the project's own note (AGENTS.md section 61.6) says
an interrupted PUTFILE can damage a FAT cluster chain -- the partial file here
deleted cleanly with CMD:RM, but that is luck, not a property of the channel.
A ~1 KB file transfers fine, which is why the earlier base-14 attempt got
through at all.

You probably do not need this file: **/pdf_english.pdf already on the card is
the same shape** -- hard-wrapped lines, line-final break hyphens
(termi-nology, elec-troencephalography, counterpro-ductive,
telecommuni-cation), two paragraphs, an embedded font, and it reflows
(`reflow page 1: 25 lines`). Use it for on-device checks and keep this
generator for the cases it cannot express (a whitespace-only blank line, an
indented paragraph start, a short heading).
"""
import os
import sys
import tempfile

# OFL-licensed, present in this workspace; only an ASCII subset is embedded.
DEFAULT_SOURCE_FONT = (
    "/Users/hongbin/Documents/projects/cpyos/repos/crossmux/lib/EpdFont/"
    "builtinFonts/source/Ubuntu/Ubuntu-Medium.ttf"
)
FIRST, LAST = 0x20, 0x7E

LINES = [
    "The reflow engine must join these three",
    "counterpro-",
    "ductive lines into a single paragraph.",
    " ",  # an OCR blank line: present but whitespace-only, not absent
    "A short heading",
    "   An indented line starts its own paragraph.",  # three spaces of indent
    "It continues here because the previous line",
    "did not end a sentence at all",
]

STEP = 22.0  # one text object per visual line, as an OCR layer writes them


def build_subset_ttf(source, out_path):
    """Return (font bytes, unitsPerEm, advances for FIRST..LAST)."""
    from fontTools.subset import Options, Subsetter
    from fontTools.ttLib import TTFont

    opts = Options()
    opts.layout_features = []
    opts.hinting = False
    opts.notdef_outline = True
    font = TTFont(source)
    subsetter = Subsetter(opts)
    subsetter.populate(unicodes=list(range(FIRST, LAST + 1)))
    subsetter.subset(font)
    font.save(out_path)

    saved = TTFont(out_path)
    cmap = saved.getBestCmap()
    hmtx = saved["hmtx"]
    upem = saved["head"].unitsPerEm
    widths = [hmtx[cmap[cp]][0] if cp in cmap else 0 for cp in range(FIRST, LAST + 1)]
    with open(out_path, "rb") as handle:
        return handle.read(), upem, widths


def escape(text):
    out = []
    for ch in text:
        if ch in "()\\":
            out.append("\\" + ch)
        elif FIRST <= ord(ch) <= LAST:
            out.append(ch)
        else:
            out.append(" ")  # the fixture is ASCII by construction
    return "".join(out)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pdf_hardwrap.pdf"
    source = os.environ.get("CPYOS_FIXTURE_FONT", DEFAULT_SOURCE_FONT)
    if not os.path.isfile(source):
        print("source font not found: %s" % source, file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        font_bytes, upem, widths = build_subset_ttf(source, os.path.join(tmp, "subset.ttf"))

    stream = "\n".join(
        "BT /F1 13 Tf 54.0 %.1f Td (%s) Tj ET" % (720.0 - i * STEP, escape(line))
        for i, line in enumerate(LINES)
    ).encode("ascii")
    width_txt = " ".join(str(int(round(w * 1000.0 / upem))) for w in widths)

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /TrueType /BaseFont /FixtureSans "
        b"/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 "
        b"/Widths [" + width_txt.encode() + b"] /FontDescriptor 6 0 R >>",
        b"<< /Type /FontDescriptor /FontName /FixtureSans /Flags 32 /ItalicAngle 0 "
        b"/FontBBox [-100 -300 1000 800] /Ascent 780 /Descent -190 /CapHeight 700 "
        b"/StemV 80 /FontFile2 7 0 R >>",
        b"<< /Length " + str(len(font_bytes)).encode() + b" /Length1 " + str(len(font_bytes)).encode()
        + b" >>\nstream\n" + font_bytes + b"\nendstream",
    ]

    buf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(buf))
        buf += ("%d 0 obj\n" % i).encode() + body + b"\nendobj\n"
    xref_at = len(buf)
    buf += ("xref\n0 %d\n" % (len(objects) + 1)).encode()
    buf += b"0000000000 65535 f \n"
    for off in offsets:
        buf += ("%010d 00000 n \n" % off).encode()
    buf += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objects) + 1, xref_at)).encode()

    with open(out_path, "wb") as handle:
        handle.write(bytes(buf))
    print("wrote %s: %d bytes (%d font bytes), %d text objects -- "
          "expect '8 blocks -> 4 paragraphs' with the option ON"
          % (out_path, len(buf), len(font_bytes), len(LINES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
