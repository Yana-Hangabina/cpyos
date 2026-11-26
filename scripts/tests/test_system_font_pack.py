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
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import font_cmap  # noqa: E402

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
        cmap = font_cmap.rendered_codepoints(UI_TTF)
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
        cmap = font_cmap.rendered_codepoints(UI_TTF)
        for codepoint in (0x00B7, 0x30FB, 0x00AB, 0x00BB, 0x2039, 0x203A, 0x2212):
            self.assertIn(codepoint, cmap)

    def test_every_ui_codepoint_has_a_face(self):
        import build_ui_font_subset

        ui = build_ui_font_subset.ui_codepoints()
        covered = font_cmap.rendered_codepoints(UI_TTF) | font_cmap.rendered_codepoints(EXT_TTF)
        missing = sorted(ui - covered)
        self.assertEqual(missing, [], 'no face can draw ' + ', '.join(f'U+{c:04X}' for c in missing[:20]))

    def test_manifest_pack_names_are_covered_by_the_ui_face(self):
        # The font-extras list prints each pack's `name` straight out of the
        # published manifest, and UI text falls back only to the 8pt emergency
        # subset -- never to the reading core. 「多语言文字（韩泰阿希梵越）」
        # therefore showed 梵 as a box on the Plus even though the 8,105 core
        # has it. ui_codepoints() unions those names; this pins it.
        import json

        import build_ui_font_subset

        spec = ROOT / 'font-extras' / 'packs.json'
        packs = json.loads(spec.read_text(encoding='utf-8')).get('packs', [])
        self.assertTrue(packs, 'packs.json declares none; the test would be vacuous')
        names = {c for pack in packs for c in pack.get('name', '') if not c.isspace()}
        ui = build_ui_font_subset.ui_codepoints()
        missing = sorted(ord(c) for c in names if ord(c) not in ui)
        self.assertEqual(missing, [], 'UI subset misses manifest name characters: '
                         + ', '.join(f'U+{cp:04X}' for cp in missing[:20]))

    def test_non_cjk_ui_is_in_the_supplement(self):
        import build_ui_font_subset

        ext = font_cmap.rendered_codepoints(EXT_TTF)
        non_cjk = {cp for cp in build_ui_font_subset.ui_codepoints()
                   if not build_ui_font_subset.is_cjk(cp)}
        self.assertEqual(sorted(non_cjk - ext), [])

    def test_a_platform31_only_reader_still_covers_the_ui(self):
        # The 8,105 face keeps astral codepoints only in its (3,10)/(0,4) subtables: measured
        # 8,848 under (3,1) against 9,044 overall. FreeType reads both, but a consumer that
        # walks only format-4 platform 3/1 -- src/JjwxcFontMap.cpp does exactly that for the
        # watermark fonts -- must see everything that face is responsible for within the BMP.
        # Checked per face: comparing one face's subtable against another face's coverage is
        # how the first version of this test "failed" on perfectly correct Latin-1/Arabic
        # codepoints that only ever lived in the world-scripts supplement.
        import build_ui_font_subset

        for path in (UI_TTF, EXT_TTF, FONT_CORE):
            if not path.is_file():
                continue
            subtables = font_cmap.cmap_subtables(path)
            narrow = subtables.get((3, 1), set())
            broad = font_cmap.rendered_codepoints(path)
            bmp_only_gaps = sorted(cp for cp in broad - narrow if cp <= 0xFFFF)
            self.assertFalse(bmp_only_gaps,
                             f'{path.name}: (3,1) misses BMP codepoints its Unicode subtables '
                             f'declare: {["U+%04X" % c for c in bmp_only_gaps[:10]]}')

        # And the UI copy itself must stay inside the BMP, or no single format-4 table can
        # serve it and the fallback chain silently picks a smaller face.
        astral = sorted(cp for cp in build_ui_font_subset.ui_codepoints() if cp > 0xFFFF)
        self.assertFalse(astral, 'app0 UI copy grew an astral codepoint: '
                                 + ', '.join(f'U+{c:04X}' for c in astral[:10]))

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

    def test_every_device_ships_the_same_core_tier(self):
        # Both devices ship the 3,500 tier; the 8,105 core is the "Chinese extras"
        # download, not a second preinstalled tier. Two shipped tiers made the extras
        # pack redundant on Plus (already 8,105) and absent on Read Pico (can only fit
        # 3,500: 1,640,306 B against ~29 KB of free spiffs).
        import generate_data_manifest as gen

        sizes = {'3500': 642887, '8105': 1640306}
        # Exactly the Read Pico and Plus partitions as shipped today.
        self.assertEqual(gen.choose_core_tier(sizes, 0x15C000, 2097152), '3500')
        self.assertEqual(gen.choose_core_tier(sizes, 0x15C000, 3145728), '3500')
        # An explicit wider preference still falls back when it does not fit, so a
        # future target can ask for 8,105 without breaking a tight partition.
        self.assertEqual(gen.choose_core_tier(sizes, 0x15C000, 2097152, preferred='8105'), '3500')
        self.assertEqual(gen.choose_core_tier(sizes, 0x15C000, 3145728, preferred='8105'), '8105')
        # Nothing fits -> no tier, which the caller must turn into a refusal rather
        # than a silent downgrade to a font the reader cannot use.
        self.assertIsNone(gen.choose_core_tier(sizes, 0x15C000, 0x15C000 + 1000))
        self.assertIsNone(gen.choose_core_tier({}, 0x15C000, 8 * 1024 * 1024))

    def test_policy_tier_wins_even_when_a_wider_one_fits(self):
        import generate_data_manifest as gen

        sizes = {'3500': 100, '8105': 200}
        # Spare space alone must not upgrade the shipped tier; the extras pack is
        # how a reader gets 8,105.
        self.assertEqual(gen.choose_core_tier(sizes, 0, 400), '3500')
        self.assertEqual(gen.choose_core_tier(sizes, 0, 400, preferred='8105'), '8105')
        # A tier that would end exactly on the last byte still counts as fitting.
        self.assertEqual(gen.choose_core_tier(sizes, 200, 400), '3500')
        self.assertEqual(gen.choose_core_tier(sizes, 201, 400), '3500')

    def test_core_carrier_round_trips_to_the_reading_font(self):
        # The candidates are the generator's own blobs. This test used to rebuild the
        # container by hand "because the host may lack fontTools", but core_carriers() needs
        # only zlib -- and the hand-built header is how a test kept passing after the carrier
        # grew a tier label in the bytes the duplicate wrote as zeros.
        sys.path.insert(0, str(ROOT / 'scripts'))
        import build_ui_font_subset as sub

        carriers = sub.core_carriers()
        for tier, source in sub.CORE_TIERS.items():
            if not source.is_file():
                continue
            self.assertEqual(inflate_carrier(carriers[tier]), source.read_bytes(),
                             f'the {tier} carrier does not inflate back to its source TTF')
        if CORE.exists():
            # The carrier on disk belongs to whichever partition was described last:
            # the 2.0 MiB Read Pico spiffs takes the 3,500 tier, the Plus' 3.125 MiB
            # takes 8,105. Both are correct, so the assertion is membership in the set
            # of valid tier carriers -- comparing against one tier would make a valid
            # Plus manifest look stale and teach people to ignore this check.
            on_disk = CORE.read_bytes()
            self.assertIn(on_disk, list(carriers.values()),
                          'build/cjk_core.dat is neither core tier; re-run '
                          'scripts/generate_data_manifest.py')
            # Whichever tier is installed must actually inflate to a TrueType face
            # with the coverage its name promises, or the release ships a font the
            # reader cannot use and the manifest still looks consistent.
            recovered = inflate_carrier(on_disk)
            self.assertTrue(recovered[:4] in (b'\x00\x01\x00\x00', b'OTTO', b'true', b'ttcf'),
                            'the core carrier does not decompress to a sfnt')


class CoreTierArtifactsTest(unittest.TestCase):
    """Each reading-core tier gets its own artifact, and manifests name it.

    One filename for two tiers was a real hazard: Read Pico's XIP layout takes the
    3,500-glyph carrier and the Plus (and the wider Read Pico layout) take the 8,105 one,
    both at the same partition offset. Generating one manifest and then the other left
    build/cjk_core.dat holding whichever ran last, so the first manifest described a blob
    that no longer existed on disk -- and the surviving file is still a valid CPFZ pack,
    so a header shape check cannot tell the two coverages apart. The tier label in the
    header (test_carrier_declares_its_own_tier) is what makes the wrong blob identifiable
    by content; the manifest's size and sha are what make it a release failure.
    """

    def _manifest(self, env):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / f'data_manifest_{env}.txt'
            subprocess.run([sys.executable, str(ROOT / 'scripts' / 'generate_data_manifest.py'),
                            '--env', env, '--out', str(out)], check=True,
                           capture_output=True, text=True, cwd=str(ROOT))
            return out.read_text(encoding='utf-8')

    def test_each_tier_has_its_own_blob(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        import build_ui_font_subset as sub

        paths = {tier: sub.core_tier_path(tier) for tier in sub.CORE_TIERS}
        self.assertEqual(sorted(path.name for path in paths.values()),
                         ['cjk_core_3500.dat', 'cjk_core_8105.dat'])
        for tier, path in paths.items():
            self.assertEqual(path.read_bytes(), sub.core_carriers()[tier],
                             f'{path.name} is not the {tier} carrier')
        self.assertNotEqual(paths['3500'].read_bytes(), paths['8105'].read_bytes())

    def test_carrier_declares_its_own_tier(self):
        """The blob says which tier it holds, so the shared alias name cannot hide it.

        cjk_core.dat is one name for two contents, chosen by the target's partition. Name
        agreement was fixed in 15.13, but that only proves the manifest and the bundler point
        at the same file -- not that the file holds the tier the writer meant. The header's
        reserved field carries the tier, so any consumer (the device log,
        exp/verify_device_packs.py, a hexdump) can tell them apart from the bytes.
        """
        sys.path.insert(0, str(ROOT / 'scripts'))
        import build_ui_font_subset as sub
        import generate_data_manifest as gen

        # One offset, three consumers: the writer here, the struct the device reads, and the
        # host tool that prints it. Reading it out of the C++ header is what makes a move by
        # any one of them a failure rather than a silent "untagged".
        offset = gen.read_cpp_constant(str(ROOT / 'src' / 'SystemCoreFontCheck.h'),
                                        'kTierFieldOffset')
        size = gen.read_cpp_constant(str(ROOT / 'src' / 'SystemCoreFontCheck.h'),
                                     'kTierFieldSize')
        self.assertEqual((offset, size), (0x14, 8), 'the documented label field moved')
        end = offset + size

        carriers = sub.core_carriers()
        self.assertEqual({tier: blob[offset:end] for tier, blob in carriers.items()},
                         {'3500': b'3500\x00\x00\x00\x00', '8105': b'8105\x00\x00\x00\x00'},
                         'the tier label is not where SystemCoreFontCheck.h reads it')
        for tier, blob in carriers.items():
            self.assertEqual(sub.core_outputs(tier)[str(sub.CORE_DAT)][offset:end],
                             blob[offset:end], f'the alias for {tier} declares another tier')
            on_disk = sub.core_tier_path(tier).read_bytes()
            self.assertEqual(on_disk[offset:end], blob[offset:end],
                             f'{sub.core_tier_path(tier).name} predates the tier label: '
                             're-run scripts/generate_data_manifest.py')
        # A label must not be the only difference between two tiers, or a corrupt label
        # would make them indistinguishable in practice.
        self.assertNotEqual(carriers['3500'][0x04:0x14], carriers['8105'][0x04:0x14])

    def test_both_layouts_publish_the_same_core_file(self):
        # Unified tier: one carrier, one name, one hash for every target -- which is
        # also what makes the shared release asset unambiguous. The tiered name stays
        # (never the bare cjk_core.dat alias) so a stale build/ file cannot be mistaken
        # for the one this manifest describes.
        xip = self._manifest('readpico_pdf_jjwxc')
        wide = self._manifest('readpico_jjwxc')
        row = re.compile(r'^data\tcore\t(\S+)\t(\S+)\t(\d+)\t(\S+)\t(\S+)$', re.M)
        a, b = row.search(xip).groups(), row.search(wide).groups()
        self.assertEqual(a[4], 'build/cjk_core_3500.dat')
        self.assertEqual(b[4], a[4], 'the two layouts disagree on the core carrier')
        self.assertEqual(a[2], b[2], 'the two layouts describe different carrier sizes')
        self.assertEqual(a[3], b[3], 'the two layouts describe different carrier hashes')
        for cells in (a, b):
            blob = ROOT / cells[4]
            self.assertEqual(blob.stat().st_size, int(cells[2]))
            self.assertEqual(hashlib.sha256(blob.read_bytes()).hexdigest(), cells[3],
                             f'{cells[4]} does not match the manifest that names it')

    def test_tier_selection_is_uniform_across_layouts(self):
        # Read Pico's XIP partition leaves ~29 KB behind the other blobs and Plus has
        # room for more, but both ship the policy tier; only a partition that cannot
        # hold it at all refuses.
        sys.path.insert(0, str(ROOT / 'scripts'))
        import generate_data_manifest as gen

        offset = gen.read_cpp_constant(str(ROOT / 'src' / 'SystemCoreFont.h'), 'kPartitionOffset')
        sizes = gen.core_tier_sizes(str(ROOT))
        self.assertEqual(gen.choose_core_tier(sizes, offset, 2 * 1024 * 1024), '3500')
        self.assertEqual(gen.choose_core_tier(sizes, offset, 3125 * 1024), '3500')
        self.assertIsNone(gen.choose_core_tier(sizes, offset, 700 * 1024))

    def test_no_tier_would_fall_back_to_the_ambiguous_alias(self):
        """Negative control: without a tier the path is the single shared name.

        That is exactly the shape that allowed one target's build to overwrite the other
        target's carrier, so it must stay visible in the code rather than accidental.
        """
        sys.path.insert(0, str(ROOT / 'scripts'))
        import generate_data_manifest as gen

        self.assertEqual(gen.core_blob_path(str(ROOT), None), str(ROOT / 'build' / 'cjk_core.dat'))
        self.assertNotEqual(gen.core_blob_path(str(ROOT), '3500'),
                            gen.core_blob_path(str(ROOT), '8105'))


class CarrierCheckWithoutFontToolsTest(unittest.TestCase):
    """`--check-carriers` must work where CI cannot install fontTools, and must still bite.

    ci.yml installs only pioarduino, so the full `--check` has never run in CI: the promise
    that cpyos_han_sans.def is the compressed form of the committed UI TTF was enforced by
    nobody. The dependency-free half closes that, provided it actually fails on a mismatch.
    """

    def setUp(self):
        try:
            import build_ui_font_subset
        except ImportError:  # pragma: no cover
            self.skipTest('build_ui_font_subset unavailable')
        self.sub = build_ui_font_subset

    def test_real_tree_passes(self):
        self.assertEqual(self.sub.carriers_only_check(), 0)

    def test_a_carrier_that_does_not_match_its_ttf_is_reported_stale(self):
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ttf = root / 'face.ttf'
            carrier = root / 'face.def'
            ttf.write_bytes(b'true type bytes')
            carrier.write_bytes(b'someone elses carrier')
            saved = (self.sub.UI_TTF, self.sub.UI_DEF)
            try:
                self.sub.UI_TTF, self.sub.UI_DEF = ttf, carrier
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = self.sub.carriers_only_check()
            finally:
                self.sub.UI_TTF, self.sub.UI_DEF = saved
            self.assertEqual(rc, 1, 'a mismatched carrier passed the check')
            self.assertIn('STALE', out.getvalue())

    def test_a_matching_carrier_pair_passes(self):
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ttf = root / 'face.ttf'
            carrier = root / 'face.def'
            raw = b'true type bytes ' * 400
            ttf.write_bytes(raw)
            carrier.write_bytes(self.sub._carrier(raw))
            saved = (self.sub.UI_TTF, self.sub.UI_DEF)
            try:
                self.sub.UI_TTF, self.sub.UI_DEF = ttf, carrier
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = self.sub.carriers_only_check()
            finally:
                self.sub.UI_TTF, self.sub.UI_DEF = saved
            self.assertEqual(rc, 0, out.getvalue())

class DocumentedCoverageTest(unittest.TestCase):
    """Pins the coverage numbers the plan and the font-extras README now assert in prose.

    15.14's whole argument -- that the field report is a registration failure, and that
    self-shipping 8,105 is a separate layout decision -- rests on which face contains which
    character. If a future subset run changes those sets, the docs go quiet-wrong; this makes
    them loud-wrong instead. The subset relation is the load-bearing one for the decision not
    to rename the SD family: 8,105 supersedes 3,500 in place only because it is a superset.
    """

    def cps(self, path):
        return set(font_cmap.rendered_codepoints(path))

    def test_face_sizes_match_the_documented_table(self):
        # The UI face is expressed against chars_ui.txt rather than an absolute count.
        # The two shipping trees legitimately differ there: dev's source scan picks up four
        # characters (吴/宇/恩/陈) that exist only because dev's main.cpp hardcodes private
        # test book titles, which public must not contain (§13.4). Pinning 1,520 would have
        # made public's own CI fail on the difference that is the correct one -- and would
        # have tempted the sync to carry those glyphs into the public tree to make it green.
        chars = {ord(c) for c in CHARSET.read_text(encoding='utf-8') if c.strip()}
        self.assertEqual(self.cps(UI_TTF), chars | {0x0020, 0x3000, 0x3007},
                         'the UI face must be exactly what this tree needs, plus space, '
                         'ideographic space and 〇 (U+3007, used in dates and numbers)')
        # The shared faces are byte-identical across the trees (verified 2026-11-26), so the
        # documented absolute counts are meaningful there.
        self.assertEqual(len(self.cps(EXT_TTF)), 438)
        self.assertEqual(len(self.cps(FONT_CORE)), 3639)
        self.assertEqual(len(self.cps(FONT_8105)), 9044)

    def test_reported_glyphs_are_core_only(self):
        ui = self.cps(UI_TTF)
        narrow = self.cps(FONT_CORE)
        wide = self.cps(FONT_8105)
        for ch in '拟勾综':
            self.assertNotIn(ord(ch), ui, f'{ch} must not be in the UI face')
            self.assertIn(ord(ch), narrow, f'{ch} must be in the 3,500 carrier')
            self.assertIn(ord(ch), wide, f'{ch} must be in the 8,105 carrier')
        for ch in '璐曦玥':
            self.assertIn(ord(ch), wide)
            self.assertNotIn(ord(ch), narrow,
                             'if this flips, Read Pico no longer needs the download for '
                             'name characters and 15.14/release notes must be rewritten')

    def test_the_wide_tier_is_a_superset_of_the_narrow_one(self):
        narrow = self.cps(FONT_CORE)
        wide = self.cps(FONT_8105)
        missing = sorted(narrow - wide)
        self.assertFalse(missing,
                         'the 8,105 pack would remove coverage: '
                         + ', '.join(f'U+{c:04X}' for c in missing[:8]))

    def test_cmap_reader_accepts_a_path_as_well_as_bytes(self):
        from_path = font_cmap.cmap_codepoints(UI_TTF)
        from_bytes = font_cmap.cmap_codepoints(UI_TTF.read_bytes())
        self.assertEqual(from_path, from_bytes)


class CmapParserEquivalenceTest(unittest.TestCase):
    """The stdlib parser carries assertions that used to skip without fontTools.

    Making the coverage contract run everywhere is only honest if the parser it now relies
    on agrees with the reference implementation. This test compares them where both exist
    and says plainly when it cannot -- it must never be read as "the contract was verified"
    on a machine that skipped it.
    """

    def test_matches_fontTools_on_the_shipping_faces(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools is absent here; the contract still runs through '
                          'font_cmap, but parser equivalence was not checked on this host')
        for path in (UI_TTF, EXT_TTF, FONT_CORE, FONT_8105):
            if not path.is_file():
                continue
            best = set(TTFont(path).getBestCmap())
            rendered = font_cmap.rendered_codepoints(path)
            self.assertEqual(best, rendered,
                             f'{path.name}: font_cmap and getBestCmap disagree; coverage '
                             'assertions built on the parser would be measuring a different set')


if __name__ == '__main__':
    unittest.main()
