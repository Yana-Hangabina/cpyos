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

    def test_the_plus_partition_still_holds_the_widest_core(self) -> None:
        # The carrier tier is chosen from the partition (scripts/generate_data_manifest.py),
        # so a table that shrinks below the 8,105 carrier does not fail a build -- it
        # quietly ships the 3,500 core and cuts Han coverage on the device that used to
        # have the most room. Read Pico genuinely cannot hold 8,105; the Plus can, and
        # that difference is the whole reason the tier is derived instead of hardcoded.
        import zlib

        raw = (ROOT / "assets/fonts/CPYOSHanSansSC-Regular-8105.ttf").read_bytes()
        packer = zlib.compressobj(9, zlib.DEFLATED, -15)
        carrier = 32 + len(packer.compress(raw) + packer.flush())
        _base, size = spiffs()
        core_off = offset(HEADERS["core"], CONSTANTS["core"])
        self.assertLessEqual(
            core_off + carrier, size,
            "the Plus data partition no longer holds the 8,105-glyph reading core "
            "(needs 0x%x bytes at 0x%x, partition is 0x%x); regenerating the manifest "
            "will fall back to the 3,500 tier" % (carrier, core_off, size))

    def test_reserved_factory_bytes_are_untouched(self) -> None:
        # pinyin ends well before the 0x80000 hole; hyphen starts after it.
        base, size = spiffs()
        reserved = 0x80000
        self.assertLess(offset(HEADERS["web"], CONSTANTS["web"]), reserved)
        self.assertGreater(offset(HEADERS["hyphen"], CONSTANTS["hyphen"]), reserved)
        self.assertLess(reserved, size)


class ManifestDrivenAddressesTest(unittest.TestCase):
    """Plus packaging/flasher must read addresses from the manifest, not remember them.

    package_metalio_plus.py used to print pinyin 0xCD0000 / i18n 0xDD0000 / emoji
    0xE30000 / core 0xE50000 as literals, and three of those were wrong once the i18n
    pack and the core tier moved -- a user following the bundle notes would write packs
    where nothing reads them, which is the same silent failure as flashing the wrong
    partition map. These tests need build artifacts, so they skip on a bare checkout.
    """

    MANIFEST = ROOT / "build/data_manifest_plus.txt"

    def setUp(self) -> None:
        if not self.MANIFEST.is_file():
            self.skipTest("no build/data_manifest_plus.txt; run scripts/generate_data_manifest.py")
        import sys

        for path in (ROOT / "scripts", ROOT / "ports/metalio_plus/scripts"):
            if str(path) not in sys.path:
                sys.path.insert(0, str(path))
        import data_manifest
        import flash_metalio_plus
        import package_metalio_plus

        self.dm = data_manifest
        self.flash = flash_metalio_plus
        self.pack = package_metalio_plus
        self.rows = package_metalio_plus.manifest_rows(ROOT)

    def test_addresses_match_the_headers(self) -> None:
        base, _size = spiffs()
        for name in CONSTANTS:
            row = next((r for r in self.rows if r["name"] == name), None)
            self.assertIsNotNone(row, f"manifest has no {name} row")
            want = base + offset(HEADERS[name], CONSTANTS[name])
            self.assertEqual(int(row["address"]), want,
                             "%s: manifest says 0x%X, headers say 0x%X"
                             % (name, int(row["address"]), want))

    def test_core_resolves_to_the_tiered_carrier(self) -> None:
        core = self.dm.blob(ROOT, self.MANIFEST, "core")
        self.assertIsNotNone(core, "manifest names no readable core blob")
        self.assertEqual(core.name, "cjk_core_8105.dat",
                         "the Plus partition holds the wide carrier; the alias cannot prove it")
        self.assertEqual(self.flash.core_blob(None), core)

    def test_pack_address_refuses_a_mismatched_blob(self) -> None:
        # Negative control: hand the packer the other tier under the core row. It is a
        # valid CPFZ carrier, so a header check would pass it -- only size/sha against the
        # manifest catches it.
        other = ROOT / "build/cjk_core_3500.dat"
        if not other.is_file():
            self.skipTest("no cjk_core_3500.dat to test the guard against")
        with self.assertRaises(SystemExit):
            self.pack.pack_address(self.rows, other, "core")

    def test_no_hardcoded_pack_addresses_remain(self) -> None:
        text = (ROOT / "ports/metalio_plus/scripts/package_metalio_plus.py").read_text(encoding="utf-8")
        for stale in ("flash at 0xDD0000", "flash at 0xE30000", "flash at 0xE50000",
                      "flash at 0xCD0000", "flash at 0xD60000"):
            self.assertNotIn(stale, text, "%s came back; addresses come from the manifest" % stale)


if __name__ == "__main__":
    unittest.main()
