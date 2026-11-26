"""The RTL/shaping fallback: id agreement, the re-binding, and the premise for both.

Arabic and Persian UI text is the one place where "the font covers the strings" is not
enough: lib/MiniBidi rewrites letters into Arabic Presentation Forms before drawing, and
no app0 face carries those forms.  The only face in the image that does is the built-in
RTL family, and SystemFontPack::addFaceId() wipes the fallback that reached it.  Three
separate facts therefore have to stay true at once, and each is pinned here:

  1. main.cpp registers the family under the id src/UiRtlFontId.h declares;
  2. typography::apply() re-attaches that id after the pack rebinds the roles;
  3. the premise -- app0 has no presentation forms, the RTL build does.

The negative controls (a doctored copy of each source must fail the same assertion) are
what make these more than grep against a string nobody ever removes.
"""

import re
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

from font_cmap import cmap_of  # noqa: E402

MAIN = ROOT / "src" / "main.cpp"
HEADER = ROOT / "src" / "UiRtlFontId.h"
TYPOGRAPHY = ROOT / "src" / "Typography.cpp"
UBUNTU_12 = ROOT / "lib" / "EpdFont" / "builtinFonts" / "ubuntu_12_regular.h"
MINIBIDI = ROOT / "lib" / "MiniBidi" / "minibidi.h"

FORM_RANGES = ((0xFB50, 0xFDFF), (0xFE70, 0xFEFF))
RTL_ID_RE = re.compile(r"kRtlFontId\s*=\s*(0x[0-9A-Fa-f]+)")
HEADER_ID_RE = re.compile(r"kFontId\s*=\s*(0x[0-9A-Fa-f]+)")


def count_in_ranges(cps):
    return sum(1 for cp in cps for lo, hi in FORM_RANGES if lo <= cp <= hi)


class RtlFallbackTest(unittest.TestCase):
    def test_header_and_main_agree_on_the_id(self):
        declared = HEADER_ID_RE.search(HEADER.read_text(encoding="utf-8"))
        registered = RTL_ID_RE.search(MAIN.read_text(encoding="utf-8"))
        self.assertIsNotNone(declared, "src/UiRtlFontId.h lost kFontId")
        self.assertIsNotNone(registered, "main.cpp no longer declares kRtlFontId")
        self.assertEqual(int(declared.group(1), 16), int(registered.group(1), 16))

    def test_header_id_is_unique_among_font_ids(self):
        """A colliding id would silently replace another face."""
        text = (ROOT / "src" / "fontIds.h").read_text(encoding="utf-8")
        values = {int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{6,}", text)}
        mine = int(HEADER_ID_RE.search(HEADER.read_text(encoding="utf-8")).group(1), 16)
        self.assertNotIn(mine, values, "the RTL id collides with a generated font id")

    def test_typography_rebinds_the_shaping_face(self):
        source = TYPOGRAPHY.read_text(encoding="utf-8")
        self.assertIn('#include "UiRtlFontId.h"', source)
        self.assertIn("ui_rtl::kFontId", source)
        bound = re.findall(r"setFallbackFont\((\w+), emergency, shaping\)", source)
        self.assertGreaterEqual(len(bound), 2, "the role loop and the reader-size loop must both bind")
        # Guarded, because a build without the high-DPI profile never registers the face.
        self.assertIn("hasFace(renderer, ui_rtl::kFontId)", source)

    def test_typography_binding_is_not_vacuous(self):
        """Negative control: the same regex over the pre-fix shape must not match."""
        before = TYPOGRAPHY.read_text(encoding="utf-8").replace(", emergency, shaping)", ", emergency)")
        self.assertEqual(re.findall(r"setFallbackFont\((\w+), emergency, shaping\)", before), [])
        self.assertIn(", emergency)", before)

    def test_main_registration_is_not_self_locking(self):
        """main.cpp must register the face before typography::apply() runs.

        apply() only binds ids that exist (hasFace), so registering it afterwards would
        leave the roles bound to nothing -- and the log would look like a working chain.
        """
        source = MAIN.read_text(encoding="utf-8")
        register = source.index("renderer.insertFont(kRtlFontId")
        calls = [m.start() for m in re.finditer(r"typography::apply\(", source)]
        self.assertTrue(calls, "main.cpp no longer calls typography::apply()")
        self.assertTrue(all(register < call for call in calls),
                        "the RTL face must be inserted before the roles are rebound")

    def test_premise_app0_has_no_presentation_forms(self):
        app0 = set()
        for name in ("CPYOSHanSansSC-UI.ttf", "CPYOSWorldScripts-UI.ttf"):
            app0 |= cmap_of(ROOT / "assets" / "fonts" / name)
        self.assertEqual(count_in_ranges(app0), 0,
                         "app0 now carries Arabic presentation forms; if that is deliberate, "
                         "the RTL fallback comment in src/UiRtlFontId.h needs updating")

    def test_premise_rtl_face_carries_them(self):
        command = UBUNTU_12.read_text(encoding="utf-8", errors="ignore").splitlines()[5]
        self.assertIn("NotoSansArabic", command, "the RTL font is no longer built from NotoSansArabic")
        intervals = [int(v, 16) for v in re.findall(r"--additional-intervals (0x[0-9A-Fa-f]+)", command)]
        forms = [v for v in intervals if any(lo <= v <= hi for lo, hi in FORM_RANGES)]
        self.assertGreaterEqual(len(forms), 8, f"presentation-form intervals missing: {forms}")

    def test_premise_shaping_rewrites_to_forms(self):
        text = MINIBIDI.read_text(encoding="utf-8")
        self.assertIn("Arabic contextual shaping (do_shape)", text)
        self.assertTrue((ROOT / "lib" / "MiniBidi" / "BidiUtils.cpp").read_text(encoding="utf-8")
                        .find("do_shape") >= 0)


if __name__ == "__main__":
    unittest.main()
