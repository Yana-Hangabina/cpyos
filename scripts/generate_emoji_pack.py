#!/usr/bin/env python3
"""Package assets/emoji/NotoEmoji-subset.ttf into build/emoji.dat.

The container is a 32-byte header plus the raw TTF, so the firmware can validate
the blob before handing it to FreeType (magic, version, size, CRC32). Reproducible:
same source bytes -> same output bytes.
"""

import argparse
import os
import struct
import sys
import zlib

HEADER = struct.Struct("<4sB3xII16x")
MAGIC = b"EMJ1"
VERSION = 1
MAX_BYTES = 0x20000


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    raise SystemExit(1)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=repo_root())
    parser.add_argument("--source", default=None)
    parser.add_argument("--out", default=None)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    source = args.source or os.path.join(args.repo, "assets", "emoji", "NotoEmoji-subset.ttf")
    out = args.out or os.path.join(args.repo, "build", "emoji.dat")
    if not os.path.exists(source):
        fail("missing %s (the Noto Emoji subset is a committed asset)" % source)
    with open(source, "rb") as fh:
        ttf = fh.read()
    if len(ttf) < 512:
        fail("%s is too small" % source)
    if len(ttf) > MAX_BYTES:
        fail("%s is %d bytes; the partition slot allows %d" % (source, len(ttf), MAX_BYTES))
    blob = HEADER.pack(MAGIC, VERSION, len(ttf), zlib.crc32(ttf) & 0xFFFFFFFF) + ttf
    if args.check:
        if not os.path.exists(out):
            fail("%s does not exist" % out)
        if open(out, "rb").read() != blob:
            fail("%s is stale; re-run scripts/generate_emoji_pack.py" % out)
        print("%s is current" % out)
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(blob)
    print("%s: %d bytes (ttf %d, crc 0x%08x)" % (out, len(blob), len(ttf), zlib.crc32(ttf) & 0xFFFFFFFF))


if __name__ == "__main__":
    main()
