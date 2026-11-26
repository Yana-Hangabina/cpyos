"""The generated language coverage table and the claims it rests on.

src/LanguageFontCoverage.h is index-addressed by the Language enum, so a wrong order is
a silent misassignment, and its central claim -- app0 already draws every system string
in all 36 languages -- is only true as long as nobody adds a translation with a script
the UI subset does not carry.  Both of those are pinned here, together with the
shaped-script flag, which is the one thing a catalogue scan provably cannot see (the
forms appear at draw time, not in the YAML).

The mutations never edit the tree of record: the rule functions are called with doctored
inputs, so a failing assertion cannot leave a modified asset behind (AGENTS.md section
38.5 -- one edit, one build).
"""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import build_language_font_coverage as cov  # noqa: E402

HEADER = ROOT / "src" / "LanguageFontCoverage.h"
I18N_KEYS = ROOT / "lib" / "I18n" / "I18nKeys.h"

# Bits a download can close, in the same order the generator writes them.
DOWNLOADABLE = cov.NEED_CARRIER | cov.NEED_WORLD_PACK | cov.NEED_CJK_PACK | cov.NEED_EMOJI_PACK


class GeneratedHeaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows, cls.layers = cov.compute()

    def test_header_is_not_stale(self):
        self.assertTrue(HEADER.exists(), "run scripts/build_language_font_coverage.py")
        self.assertEqual(
            HEADER.read_text(encoding="utf-8"),
            cov.render(self.rows),
            "src/LanguageFontCoverage.h does not match the translations and assets; "
            "regenerate it",
        )

    def test_enum_order_matches_i18nkeys(self):
        """The table is indexed by enum value, so order is the whole contract.

        gen_i18n.py sorts by _bcp47 with English first; the generator reimplements that
        rule.  If either copy drifts from the enum actually compiled into the firmware,
        every language silently inherits another language's mask.
        """
        text = I18N_KEYS.read_text(encoding="utf-8")
        block = re.search(r"enum class Language : uint8_t \{(.*?)\};", text, re.S)
        self.assertIsNotNone(block, "no Language enum in I18nKeys.h")
        declared = re.findall(r"^\s*([A-Z0-9_]+)\s*=\s*(\d+),", block.group(1), re.M)
        names = [name for name, _ in declared]
        self.assertEqual([str(row["name"]) for row in self.rows], names)
        self.assertEqual([int(index) for _, index in declared], list(range(len(names))))

    def test_every_language_is_drawable_from_app0(self):
        """The section 15.8 finding, as a regression guard.

        app0 carries Tier 0 + Tier 0b, which were subset from these very catalogues, so
        there is no codepoint gap for system UI text in any language today.  If a future
        translation introduces a script the subset build does not carry, this fails and
        says which language -- and only then does the blocking font gate of plan section
        6.3 have something to gate on.
        """
        downloadable = DOWNLOADABLE
        offenders = [f'{row["name"]}={row["residual"]}' for row in self.rows if row["mask"] & downloadable]
        self.assertEqual(offenders, [], f"app0 no longer covers these languages: {offenders}")

    def test_no_language_has_unfixable_glyphs(self):
        offenders = [str(row["name"]) for row in self.rows if row["mask"] & cov.K_UNFIXABLE]
        self.assertEqual(offenders, [])
        self.assertEqual([row["unfixable"] for row in self.rows], [0] * len(self.rows))

    def test_header_compiles_and_its_predicates_are_constexpr(self):
        """The one check that catches a malformed generated header.

        Every other assertion here reads the generator's own data structures, so a typo in
        the emitted C++ -- an identifier renamed in one place, a brace pair lost inside the
        f-string -- would pass them all and break the firmware build.  A firmware build is
        not always available (one tree, one build at a time), so the header gets compiled
        here instead, with the predicates asserted as constant expressions.
        """
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("no C++ compiler on PATH")
        source = f"""
#include "LanguageFontCoverage.h"
static_assert(language_font::kCount == {len(self.rows)}, "row count");
static_assert(!language_font::gateNeeded(language_font::RU), "no download needed today");
static_assert(language_font::mustRefuse(language_font::AR), "arabic is shaped");
static_assert(language_font::mustRefuse(language_font::FA), "persian is shaped");
static_assert(!language_font::mustRefuse(language_font::HE), "hebrew draws as stored");
static_assert(language_font::isRtlScript(language_font::HE), "hebrew is rtl");
static_assert(language_font::needsShapingFace(language_font::AR), "arabic needs the RTL face");
static_assert(!language_font::needsShapingFace(language_font::VI), "vietnamese does not");
static_assert(language_font::maskFor(language_font::VI) == 0, "vietnamese is covered by app0");
int main() {{ return language_font::needFor(language_font::ZH_CN).mask == 0 ? 0 : 1; }}
"""
        with tempfile.TemporaryDirectory() as tmp:
            cu = Path(tmp) / "check.cpp"
            exe = Path(tmp) / "check"
            cu.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", f"-I{ROOT / 'src'}", str(cu), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, f"generated header does not compile:\n{build.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, "a constexpr assertion about the table failed")

    def test_endonyms_are_covered_too(self):
        """LANGUAGE_NAMES holds endonyms; the picker draws them before the switch.

        The generator excludes _language_name from a language's own requirement (the
        gate must be readable in the *previous* language), which would hide a gap in the
        list itself.  Measured directly here instead.
        """
        app0 = self.layers["tier0"] | self.layers["tier0b"]
        missing = {}
        for path in sorted(cov.TRANSLATIONS.glob("*.yaml")):
            text = path.read_text(encoding="utf-8")
            code = re.search(r'^_language_code:\s*"([A-Za-z0-9_]+)"', text, re.M).group(1)
            name = re.search(r'^_language_name:\s*"(.*)"', text, re.M).group(1)
            gap = {ord(c) for c in name} - app0
            if gap:
                missing[code] = sorted(f"U+{cp:04X}" for cp in gap)
        self.assertEqual(missing, {}, f"picker would draw boxes for {missing}")

    def test_rtl_and_joining_flags_have_the_right_membership(self):
        rtl = {str(row["name"]) for row in self.rows if row["mask"] & cov.K_RTL}
        self.assertEqual(rtl, {"AR", "FA", "HE"})
        joining = {str(row["name"]) for row in self.rows if row["mask"] & cov.K_JOINING}
        # Hebrew letters are drawn as stored, so it must NOT be refused: only the
        # Arabic-joining languages lose the "a download would fix it" claim.
        self.assertEqual(joining, {"AR", "FA"})




class RuleTest(unittest.TestCase):
    """The rule functions, driven with doctored inputs (tree untouched)."""

    @classmethod
    def setUpClass(cls):
        cls.rows, cls.layers = cov.compute()
        cls.required = cov.required_codepoints()

    def residual(self, code, drop_from_app0=frozenset()):
        app0 = (self.layers["tier0"] | self.layers["tier0b"]) - set(drop_from_app0)
        return {cp for cp in self.required[code] if cp > 0x7F and cp not in app0}

    def test_baseline_masks(self):
        for code in ("EN", "ZH_CN", "RU", "VI"):
            mask, _ = cov.classify(self.residual(code), self.layers)
            self.assertEqual(mask, 0, code)

    def test_mutations_are_detected(self):
        # 1. Punch a Cyrillic hole in the UI-script face: Russian must start needing a
        #    pack, i.e. the mask really is computed from the faces and not hardcoded.
        cyrillic = next(cp for cp in self.required["RU"] if 0x0400 <= cp <= 0x04FF)
        mask, _ = cov.classify(self.residual("RU", [cyrillic]), self.layers)
        self.assertEqual(mask, cov.NEED_WORLD_PACK,
                         "a hole in app0's Cyrillic must ask for the world-script pack")
        # 2. Vietnamese-specific diacritic: same wiring, different script block.
        vi_mark = next(cp for cp in self.required["VI"] if cp > 0x7F and 0x1EA0 <= cp <= 0x1EFF)
        mask, _ = cov.classify(self.residual("VI", [vi_mark]), self.layers)
        self.assertEqual(mask, cov.NEED_WORLD_PACK,
                         "a hole in app0's Vietnamese letters must ask for the pack")
        # 2b. A Han ideograph the UI uses: the cheapest fix is the partition carrier, not
        #     a download. Nothing in the shipped table exercises this bit (app0 covers
        #     every string), so without this case it would be untested arithmetic.
        han = next(cp for cp in self.required["ZH_CN"] if 0x4E00 <= cp <= 0x9FFF)
        mask, _ = cov.classify(self.residual("ZH_CN", [han]), self.layers)
        self.assertEqual(mask, cov.NEED_CARRIER, "a Han hole must ask for the core carrier")
        # 3. Remove the *only* sources of an Arabic letter: it must become unfixable,
        #    which is what stops the gate from offering a doomed download.
        arabic = next(cp for cp in self.required["AR"] if 0x0600 <= cp <= 0x06FF)
        starved = dict(self.layers)
        for name in ("core3500", "core8105", "world", "cjk_full", "emoji"):
            starved[name] = self.layers[name] - {arabic}
        mask, unfixable = cov.classify({arabic}, starved)
        self.assertTrue(mask & cov.K_UNFIXABLE, "a glyph no source carries is not unfixable")
        self.assertEqual(unfixable, 1)
        # 4. A codepoint only the emoji pack has: the pack that fixes it is the one
        #    named, otherwise an emoji in a catalogue would be reported unfixable.
        starved_all_but_emoji = {name: (self.layers[name] - {0x1F600}) for name in self.layers}
        starved_all_but_emoji["emoji"] = self.layers["emoji"] | {0x1F600}
        mask, _ = cov.classify({0x1F600}, starved_all_but_emoji)
        self.assertEqual(mask, cov.NEED_EMOJI_PACK)
        # 5. Negative control on the joining table: with the ranges emptied, Arabic loses
        #    the flag, so it can only come from that table.
        saved = cov.JOINING_RANGES
        try:
            cov.JOINING_RANGES = ()
            self.assertFalse(cov.needs_shaping(
                {cp for cp in self.required["AR"] if 0x0600 <= cp <= 0x06FF}))
        finally:
            cov.JOINING_RANGES = saved
        self.assertTrue(cov.needs_shaping({0x0628}))
        # Hebrew is right-to-left but not contextual: its letters are drawn as stored, so
        # it must not be lumped in with the languages a download cannot rescue.
        self.assertFalse(cov.needs_shaping({0x05D0}))
        self.assertTrue(cov.is_rtl({0x05D0}))
        self.assertFalse(cov.needs_shaping({0x0410}))  # Cyrillic lays out fine

    def test_gate_predicate_agrees_with_masks(self):
        """Nothing is gate-able today (app0 covers every catalogue), and the joining
        languages must never be routed to a download even if that changes."""
        for row in self.rows:
            mask = int(row["mask"])
            downloadable = bool(mask & DOWNLOADABLE)
            refusible = bool(mask & (cov.K_JOINING | cov.K_UNFIXABLE))
            expected = downloadable and not refusible
            self.assertFalse(expected, f'{row["name"]} claims a download is needed')
            if row["name"] in ("AR", "FA"):
                self.assertTrue(refusible, f'{row["name"]} would be sent to a font download')
            if row["name"] == "HE":
                self.assertFalse(refusible, "Hebrew is drawable from app0 and must stay selectable")


if __name__ == "__main__":
    unittest.main()
