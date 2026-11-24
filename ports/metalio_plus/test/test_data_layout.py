"""Pin the Plus data-partition layout derived from the firmware headers.

The manifest generator reads the same constants, but it only runs when data
blobs are built.  This test fails as soon as a header moves an offset past the
partition or into another blob, independent of any build output.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSV = ROOT / "ports/metalio_plus/partitions_plus_ota.csv"

HEADERS = {
    "pinyin": None,  # fixed at partition offset 0 by PinyinDb
    "web": ROOT / "src/network/WebPackFormat.h",
    "hyphen": ROOT / "lib/Epub/Epub/hyphenation/HyphenationStore.h",
    "i18n": ROOT / "lib/I18n/I18nPack.h",
    "emoji": ROOT / "src/EmojiPackFormat.h",
    "core": ROOT / "src/SystemCoreFont.h",
}
CONSTANTS = {
    "web": "WEB_PACK_PARTITION_OFFSET",
    "hyphen": "HYPN_PARTITION_OFFSET",
    "i18n": "I18N_PACK_PARTITION_OFFSET",
    "emoji": "kPartitionOffset",
    "core": "kPartitionOffset",
}


def spiffs() -> tuple[int, int]:
    for line in CSV.read_text(encoding="utf-8").splitlines():
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 5 and fields[1] == "data" and fields[2] == "spiffs":
            return int(fields[3], 0), int(fields[4], 0)
    raise AssertionError("%s has no data/spiffs row" % CSV)


def offset(header: Path, name: str) -> int:
    match = re.search(r"\b%s\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)u?\s*;" % name,
                      header.read_text(encoding="utf-8"))
    if not match:
        raise AssertionError("cannot find %s in %s" % (name, header))
    return int(match.group(1), 0)


class DataLayoutTest(unittest.TestCase):
    def test_offsets_are_ordered_and_inside_the_partition(self) -> None:
        base, size = spiffs()
        self.assertEqual(base, 0xCD0000, "the Plus dual-OTA table moved; update this test")
        seen = [("pinyin", 0)]
        for name in ("web", "hyphen", "i18n", "emoji", "core"):
            seen.append((name, offset(HEADERS[name], CONSTANTS[name])))
        for (prev, a), (name, b) in zip(seen, seen[1:]):
            self.assertLess(a, b, "%s (0x%x) must follow %s (0x%x)" % (name, b, prev, a))
            self.assertLess(b, size, "%s (0x%x) is outside the 0x%x partition" % (name, b, size))

    def test_reserved_factory_bytes_are_untouched(self) -> None:
        # pinyin ends well before the 0x80000 hole; hyphen starts after it.
        base, size = spiffs()
        reserved = 0x80000
        self.assertLess(offset(HEADERS["web"], CONSTANTS["web"]), reserved)
        self.assertGreater(offset(HEADERS["hyphen"], CONSTANTS["hyphen"]), reserved)
        self.assertLess(reserved, size)


if __name__ == "__main__":
    unittest.main()
