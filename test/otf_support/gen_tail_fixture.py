#!/usr/bin/env python3
"""Build the split-stream fixture for OtfStreamHostTest from cff_fixture.otf.

The firmware never hands FreeType a whole font buffer: streamed SD faces go
through a RAM prefix cache (SdCardFontSystem::prefixRead), so CFF's
end-of-file tables (CharStrings / charset / FDArray) are read in pieces that
straddle the cache boundary. To exercise that in the host test we need a CFF
fixture whose table payload sits past a small prefix; padding the sfnt pushes
the table offsets back so the read pattern is representative.

usage: gen_tail_fixture.py <in.otf> <out.otf> [pad-bytes]
Requires fontTools (macOS system python3 has it; otherwise the stream test
skips, matching the runner's other optional fixtures).
"""
import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.DefaultTable import DefaultTable


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    pad = int(sys.argv[3]) if len(sys.argv) > 3 else 96 * 1024

    font = TTFont(src)
    table = DefaultTable("PRIV")
    table.data = b"\x00" * pad
    font["PRIV"] = table
    font.save(dst)

    out = TTFont(dst)
    offsets = {tag: out.reader.tables[tag].offset for tag in out.reader.tables}
    print(f"tail fixture: {dst} ({pad} B pad)")
    for tag in ("head", "cmap", "CFF ", "PRIV"):
        if tag in offsets:
            print(f"  {tag:6} @ {offsets[tag]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
