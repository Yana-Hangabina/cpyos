#!/usr/bin/env python3
"""The GC2145 VGA table must stay byte-identical to the vendor source.

The camera is the one place where a silent register change produces a black or
garbled image with no software error. Pin the count and hash, and compare
against the official header when the vendor checkout is present.
"""

import hashlib
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
SOURCE = PORT / "components" / "cpyos_metalio_bsp" / "plus_camera.c"
ROOT = PORT.parents[3]
VENDOR_ROOT = ROOT / "repos" / "metalio-e-ink4-plus"
if not VENDOR_ROOT.is_dir():
    VENDOR_ROOT = ROOT / ".scratch" / "refs" / "metalio-vendor"
VENDOR = (VENDOR_ROOT / "main" / "boards" / "common" / "camera" / "gc2145_vga_settings.h")

ENTRY = re.compile(r"\{0x([0-9a-fA-F]{2})\s*,\s*0x([0-9a-fA-F]{2})\}")
EXPECTED_COUNT = 586
EXPECTED_SHA256 = "a676ce4d31726d85b99054124d9e6ddbca9c7a64a0ca369cf565fea13fb42067"


def table(path: Path) -> list[tuple[int, int]]:
    text = path.read_text(encoding="utf-8")
    body = text.split("kGc2145Vga[] = {", 1)[1].split("};", 1)[0]
    return [(int(a, 16), int(b, 16)) for a, b in ENTRY.findall(body)]


class CameraTableTest(unittest.TestCase):
    def test_count_and_hash_are_pinned(self):
        entries = table(SOURCE)
        self.assertEqual(len(entries), EXPECTED_COUNT)
        digest = hashlib.sha256(bytes(v for pair in entries for v in pair)).hexdigest()
        self.assertEqual(digest, EXPECTED_SHA256, "GC2145 table changed; re-verify against the vendor header")
        self.assertEqual(entries[:4], [(0xFE, 0xF0), (0xFF, 0x10), (0xFC, 0x06), (0xF6, 0x00)])
        self.assertEqual(entries[-2:], [(0x21, 0x14), (0xFE, 0x00)])

    def test_matches_the_vendor_header(self):
        if not VENDOR.is_file():
            self.skipTest(f"vendor checkout not present: {VENDOR}")
        self.assertEqual(table(SOURCE), table(VENDOR))


if __name__ == "__main__":
    unittest.main()
