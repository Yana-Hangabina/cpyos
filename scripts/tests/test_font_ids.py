"""Check the generated font-ID header, its inputs and its failsafe behaviour.

The header is the single source of every renderer font id (see
`scripts/build_font_ids.py`). These cases pin the three properties the design
depends on:

* the committed header is exactly what the generator produces;
* every id is non-zero and distinct (a collision draws one role in another
  face, silently);
* a pack change re-keys only the pack-derived reader sizes, and a built-in
  header change re-keys only that family - that is what makes pagination
  rebuild exactly once instead of never.
"""
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_font_ids", ROOT / "scripts" / "build_font_ids.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def copy_inputs(directory: Path) -> tuple[Path, Path]:
    """Stage the generator's inputs under a temporary root and return the paths."""
    builtin_dir = directory / "builtinFonts"
    builtin_dir.mkdir()
    for name, filenames in generator.HEADER_FAMILIES.items():
        for filename in filenames:
            shutil.copy2(ROOT / "lib" / "EpdFont" / "builtinFonts" / filename, builtin_dir / filename)
    pack_font = directory / "pack.ttf"
    shutil.copy2(generator.PACK_FONT, pack_font)
    return builtin_dir, pack_font


def append_byte(path: Path) -> None:
    with path.open("ab") as output:
        output.write(b"\x00")


class FontIdsTest(unittest.TestCase):
    def test_generated_header_matches_committed_file(self):
        ids = generator.font_ids()
        self.assertEqual(generator.render_header(ids), generator.OUTPUT.read_text())

    def test_committed_header_is_current_on_disk(self):
        # Same contract as `python3 scripts/build_font_ids.py --check`.
        self.assertEqual(generator.main(["--check"]), 0)

    def test_ids_are_non_zero_and_distinct(self):
        ids = generator.font_ids()
        self.assertEqual(len(ids), len(generator.HEADER_FAMILIES) + len(generator.FROZEN_IDS)
                         + len(generator.CJK_UI_IDS) + 1)
        self.assertNotIn(0, ids.values())
        self.assertEqual(len(set(ids.values())), len(ids))

    def test_pack_change_rekeys_only_pack_derived_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            builtin_dir, pack_font = copy_inputs(Path(temporary))
            baseline = generator.font_ids(builtin_dir, pack_font)
            append_byte(pack_font)
            updated = generator.font_ids(builtin_dir, pack_font)
        self.assertNotEqual(updated["CJK_PACK_SIGNATURE"], baseline["CJK_PACK_SIGNATURE"])
        for name in baseline:
            if name == "CJK_PACK_SIGNATURE":
                continue
            self.assertEqual(updated[name], baseline[name], name)

    def test_header_change_rekeys_only_that_family(self):
        with tempfile.TemporaryDirectory() as temporary:
            builtin_dir, pack_font = copy_inputs(Path(temporary))
            baseline = generator.font_ids(builtin_dir, pack_font)
            append_byte(builtin_dir / "notosans_12_regular.h")
            updated = generator.font_ids(builtin_dir, pack_font)
        self.assertNotEqual(updated["NOTOSANS_12_FONT_ID"], baseline["NOTOSANS_12_FONT_ID"])
        for name in baseline:
            if name == "NOTOSANS_12_FONT_ID":
                continue
            self.assertEqual(updated[name], baseline[name], name)

    def test_retired_serif_ids_are_frozen_literals(self):
        # The NotoSerif reader faces are retired; the ids must survive for stored
        # `fontFamily` values and the emergency fallback chain.
        self.assertEqual(generator.font_ids()["NOTOSERIF_12_FONT_ID"],
                         generator.FROZEN_IDS["NOTOSERIF_12_FONT_ID"])

    def test_retired_serif_reader_headers_are_gone(self):
        # Nothing compiled them and nothing registered them; the ids above are
        # the only thing that had to survive.
        builtin = ROOT / "lib" / "EpdFont" / "builtinFonts"
        for size in (12, 14, 16, 18):
            for style in ("regular", "bold", "bolditalic", "italic"):
                self.assertFalse((builtin / f"notoserif_{size}_{style}.h").exists(), (size, style))
        self.assertTrue((builtin / "notoserif_title_23.h").exists())

    def test_missing_header_fails_instead_of_emitting_an_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileNotFoundError):
                generator.font_ids(Path(temporary), generator.PACK_FONT)

    def test_missing_pack_fails_instead_of_emitting_a_signature(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileNotFoundError):
                generator.font_ids(generator.BUILTIN_DIR, Path(temporary) / "absent.ttf")

    def test_reader_ids_encode_the_signature_and_the_point_size(self):
        signature = generator.pack_signature()
        self.assertEqual(signature & ~0xFFFF, 0)
        for point in (generator.READER_MIN_PT, 18, generator.READER_MAX_PT):
            value = 0x43000000 | (signature << 8) | point
            self.assertEqual(value & 0xFF, point)
            self.assertEqual((value >> 8) & 0xFFFF, signature)
            self.assertLess(value, 2**31)


if __name__ == "__main__":
    unittest.main()
