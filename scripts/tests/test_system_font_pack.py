"""The committed font asset, its embedded carrier and the C3 spiffs budget.

Two copies of the same font exist on purpose: the TTF is the source for the C3
pack, and the `.def` carrier is linked into Read Pico app0. This test fails if
one is edited without the other, if the carrier does not inflate back to the
TTF, or if the C3 pack no longer fits the spiffs partition.
"""

import csv
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

FONT = ROOT / 'assets' / 'fonts' / 'CPYOSHanSansSC-Regular-8105.ttf'
CARRIER = ROOT / 'assets' / 'fonts' / 'cpyos_han_sans.def'
CHARSET = ROOT / 'assets' / 'fonts' / 'chars_8105.txt'
EXTRA = ROOT / 'assets' / 'fonts' / 'chars_extra.txt'


def spiffs_size():
    with (ROOT / 'partitions.csv').open(newline='') as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith('#')):
            if len(row) >= 5 and row[0].strip() == 'spiffs':
                return int(row[4].strip(), 0)
    raise AssertionError('partitions.csv has no spiffs row')


class SystemFontPackTest(unittest.TestCase):
    def test_carrier_inflates_to_the_committed_ttf(self):
        font = FONT.read_bytes()
        blob = CARRIER.read_bytes()
        magic, version, header_size, raw_size, raw_crc, compressed_size = struct.unpack(
            '<4sHHIII', blob[:20])
        self.assertEqual(magic, b'CPFZ')
        self.assertEqual(version, 1)
        self.assertEqual(header_size, 32)
        self.assertEqual(raw_size, len(font))
        self.assertEqual(compressed_size, len(blob) - 32)
        self.assertEqual(raw_crc, zlib.crc32(font) & 0xFFFFFFFF)

        inflater = zlib.decompressobj(-15)
        raw = inflater.decompress(blob[32:]) + inflater.flush()
        self.assertEqual(raw, font)
        self.assertTrue(inflater.eof)
        self.assertEqual(raw[:4], b'\x00\x01\x00\x00')

    def test_c3_pack_fits_spiffs_with_headroom(self):
        # The C3 installer writes the CPFP wrapper at the start of spiffs.
        font = FONT.read_bytes()
        self.assertLess(len(font) + 32, spiffs_size() * 0.9)

    def test_charset_is_covered(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:  # host test run without the font toolchain
            self.skipTest('fontTools not installed')
        cmap = TTFont(FONT).getBestCmap()
        chars = {c for c in CHARSET.read_text(encoding='utf-8') if c.strip()}
        self.assertEqual(len(chars), 8105)
        missing = sorted(c for c in chars if ord(c) not in cmap)
        self.assertEqual(missing, [])

    def test_byline_punctuation_is_covered(self):
        # Home bylines and the "progress · date" line contain U+00B7. If the
        # system face misses it, resolveTextFontId falls the whole string back
        # to the 8pt emergency face, which is exactly the "small author/date"
        # regression seen on device.
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        cmap = TTFont(FONT).getBestCmap()
        # U+00B7 byline separator plus the navigation glyphs used by STR_BACK/
        # STR_EXIT/STR_HOME/STR_PREV/STR_NEXT, the date hint's minus sign and
        # the settings picker's single guillemets (row chevrons).
        for codepoint in (0x00B7, 0x30FB, 0x00AB, 0x00BB, 0x2039, 0x203A, 0x2212):
            self.assertIn(codepoint, cmap)

    def test_extra_codepoints_are_covered(self):
        # Locale letters the 8,105-Han contract cannot hold live in
        # chars_extra.txt (Azerbaijani/Turkish: Ə/ə, ÇÖÜçöğİıŞş). One missing
        # codepoint makes resolveTextFontId fall the whole line back to 8pt,
        # so this is a rendering regression, not a cosmetic omission.
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        font = TTFont(FONT)
        cmap = font.getBestCmap()
        chars = [c for c in EXTRA.read_text(encoding='utf-8').splitlines()
                 if c.strip() and not c.lstrip().startswith('#')]
        self.assertTrue(chars)
        missing = [c for c in chars if ord(c) not in cmap]
        self.assertEqual(missing, [])
        for codepoint in (0x018F, 0x0259):
            self.assertIn(codepoint, cmap)
        # The schwa letters must be real outlines, not cmap aliases of E/e.
        for codepoint, lookalike in ((0x018F, 'E'), (0x0259, 'e')):
            glyph = font['glyf'][cmap[codepoint]]
            self.assertNotEqual(cmap[codepoint], cmap[ord(lookalike)])
            self.assertGreater(glyph.numberOfContours, 0)
        # U+018F is a capital and U+0259 a lowercase letter: their ink must sit
        # at different heights, otherwise one of them is the wrong outline.
        capital = font['glyf'][cmap[0x018F]]
        lowercase = font['glyf'][cmap[0x0259]]
        self.assertGreater(capital.yMax, lowercase.yMax)

    def test_font_ids_track_the_committed_pack(self):
        # CJK_READER_SIZE_FONT_ID embeds crc32(pack TTF) & 0xFFFF. If the pack
        # is edited without regenerating src/fontIds.h, pagination keeps the old
        # metrics; `scripts/build_font_ids.py --check` is what CI runs.
        sys.path.insert(0, str(SCRIPTS))
        import build_font_ids

        header = (ROOT / 'src' / 'fontIds.h').read_text()
        signature = build_font_ids.pack_signature()
        self.assertIn(f'#define CJK_PACK_SIGNATURE (0x{signature:04X})', header)

    def test_generator_round_trips(self):
        import build_system_font_pack

        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'system_font.dat'
            embedded = Path(temp) / 'font.def'
            sys.argv = [
                'build_system_font_pack.py', '--font', str(FONT),
                '--out', str(out), '--embedded-out', str(embedded),
            ]
            build_system_font_pack.main()
            self.assertEqual(embedded.read_bytes(), CARRIER.read_bytes())
            self.assertEqual(out.read_bytes()[32:], FONT.read_bytes())


if __name__ == '__main__':
    unittest.main()
