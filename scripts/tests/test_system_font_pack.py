"""Committed font assets, their carriers and the C3 spiffs budget.

Three products come out of the same source family:

  CPYOSHanSansSC-UI.ttf + cpyos_han_sans.def   app0 UI subset (UI + non-Han)
  CPYOSWorldScripts-UI.ttf + cpyos_ui_ext.def  non-CJK UI supplement
  CPYOSHanSansSC-Regular-8105.ttf               C3 spiffs pack + the core carrier

This test fails if an asset is edited without rebuilding its carrier, if a
carrier does not inflate back to its TTF, or if the UI coverage contract drifts
from the translation catalogs and hard-coded UI strings.
"""

import csv
import struct
import sys
import unittest
import zlib
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

FONT_CORE = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Core.ttf"
FONT_8105 = ROOT / 'assets' / 'fonts' / 'CPYOSHanSansSC-Regular-8105.ttf'
UI_TTF = ROOT / 'assets' / 'fonts' / 'CPYOSHanSansSC-UI.ttf'
UI_CARRIER = ROOT / 'assets' / 'fonts' / 'cpyos_han_sans.def'
EXT_TTF = ROOT / 'assets' / 'fonts' / 'CPYOSWorldScripts-UI.ttf'
EXT_CARRIER = ROOT / 'assets' / 'fonts' / 'cpyos_ui_ext.def'
CHARSET = ROOT / 'assets' / 'fonts' / 'chars_ui.txt'
CORE = ROOT / 'build' / 'cjk_core.dat'


def spiffs_size():
    with (ROOT / 'partitions.csv').open(newline='') as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith('#')):
            if len(row) >= 5 and row[0].strip() == 'spiffs':
                return int(row[4].strip(), 0)
    raise AssertionError('partitions.csv has no spiffs row')


def inflate_carrier(blob):
    magic, version, header_size, raw_size, raw_crc, compressed_size = struct.unpack(
        '<4sHHIII', blob[:20])
    assert magic == b'CPFZ'
    assert version == 1
    assert header_size == 32
    assert compressed_size == len(blob) - 32
    inflater = zlib.decompressobj(-15)
    raw = inflater.decompress(blob[32:]) + inflater.flush()
    assert inflater.eof
    assert len(raw) == raw_size
    assert raw_crc == zlib.crc32(raw) & 0xFFFFFFFF
    return raw


class SystemFontPackTest(unittest.TestCase):
    def test_ui_carrier_inflates_to_the_committed_ttf(self):
        font = UI_TTF.read_bytes()
        self.assertEqual(inflate_carrier(UI_CARRIER.read_bytes()), font)
        self.assertEqual(font[:4], b'\x00\x01\x00\x00')

    def test_ext_carrier_inflates_to_the_committed_ttf(self):
        font = EXT_TTF.read_bytes()
        self.assertEqual(inflate_carrier(EXT_CARRIER.read_bytes()), font)
        # The UI supplement is a small script subset, not another CJK face.
        self.assertLess(len(font), 80 * 1024)

    def test_c3_pack_fits_spiffs_with_headroom(self):
        # The C3 installer writes the CPFP wrapper at the start of spiffs.
        font = FONT_8105.read_bytes()
        self.assertLess(len(font) + 32, spiffs_size() * 0.9)

    def test_charset_is_covered(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:  # host test run without the font toolchain
            self.skipTest('fontTools not installed')
        cmap = TTFont(UI_TTF).getBestCmap()
        chars = {c for c in CHARSET.read_text(encoding='utf-8') if c.strip()}
        self.assertGreater(len(chars), 1000)
        self.assertLess(len(chars), 2500)
        missing = sorted(c for c in chars if ord(c) not in cmap)
        self.assertEqual(missing, [])

    def test_byline_punctuation_is_covered(self):
        # Home bylines and the "progress · date" line contain U+00B7. If the
        # system face misses it, resolveTextFontId falls the whole string back
        # to a smaller emergency face, which is the "small author/date"
        # regression seen on device.
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        cmap = TTFont(UI_TTF).getBestCmap()
        for codepoint in (0x00B7, 0x30FB, 0x00AB, 0x00BB, 0x2039, 0x203A, 0x2212):
            self.assertIn(codepoint, cmap)

    def test_every_ui_codepoint_has_a_face(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        import build_ui_font_subset

        ui = build_ui_font_subset.ui_codepoints()
        covered = set(TTFont(UI_TTF).getBestCmap()) | set(TTFont(EXT_TTF).getBestCmap())
        missing = sorted(ui - covered)
        self.assertEqual(missing, [], 'no face can draw ' + ', '.join(f'U+{c:04X}' for c in missing[:20]))

    def test_non_cjk_ui_is_in_the_supplement(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        import build_ui_font_subset

        ext = set(TTFont(EXT_TTF).getBestCmap())
        non_cjk = {cp for cp in build_ui_font_subset.ui_codepoints()
                   if not build_ui_font_subset.is_cjk(cp)}
        self.assertEqual(sorted(non_cjk - ext), [])

    def test_committed_assets_match_the_generator(self):
        try:
            import fontTools  # noqa: F401
        except ImportError:
            self.skipTest('fontTools not installed')
        import build_ui_font_subset

        generated = build_ui_font_subset.build(core=False)
        for name, data in generated.items():
            with self.subTest(asset=Path(name).name):
                self.assertEqual(Path(name).read_bytes(), data,
                                 f'{name} is stale; run scripts/build_ui_font_subset.py')

    def test_core_carrier_round_trips_to_the_reading_font(self):
        # Deliberately does not import the generator: the core carrier is plain
        # zlib, so this test runs on hosts without fontTools (the release
        # manifest rebuilds the core the same way).
        raw = FONT_CORE.read_bytes()
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = compressor.compress(raw) + compressor.flush()
        blob = (b'CPFZ' + struct.pack('<HHIII', 1, 32, len(raw), crc, len(packed)) + bytes(12) + packed)
        self.assertEqual(inflate_carrier(blob), raw)
        if CORE.exists():
            self.assertEqual(CORE.read_bytes(), blob,
                             'build/cjk_core.dat is stale; re-run scripts/generate_data_manifest.py')


if __name__ == '__main__':
    unittest.main()
