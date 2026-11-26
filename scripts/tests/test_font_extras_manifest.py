import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build_font_extras_manifest.py"
sys.path.insert(0, str(ROOT / "scripts"))
import build_font_extras_manifest as bfe  # noqa: E402  (shared regexes, not restated here)


class FontExtrasManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "Ext.ttf").write_bytes(b"\x00\x01\x00\x00" + b"x" * 508)
        (self.dir / "Emoji.ttf").write_bytes(b"\x00\x01\x00\x00" + b"e" * 1020)
        self.spec = {
            "packs": [
                {
                    "id": "emoji-full",
                    "kind": "emoji",
                    "family": "-",
                    "name": "Emoji",
                    "file": "Emoji.ttf",
                    "url": "https://github.com/o/r/releases/download/f/Emoji.ttf",
                },
                {
                    "id": "ext",
                    "kind": "fallback",
                    "family": "CPYOSExt",
                    "name": "Ext",
                    "file": "Ext.ttf",
                    "url": "https://github.com/o/r/releases/download/f/Ext.ttf",
                },
            ]
        }

    def tearDown(self):
        self.tmp.cleanup()

    def run_build(self, spec=None, out=None, check=False, fmt=None):
        spec_path = self.dir / "packs.json"
        spec_path.write_text(json.dumps(spec if spec is not None else self.spec), encoding="utf-8")
        cmd = [sys.executable, str(SCRIPT), "--spec", str(spec_path), "--assets-dir", str(self.dir)]
        if out is not None:
            cmd += ["--out", str(out)]
        if fmt is not None:
            cmd += ["--format", fmt]
        if check:
            cmd += ["--check"]
        return subprocess.run(cmd, text=True, capture_output=True)

    def test_builds_manifest_with_size_and_sha(self):
        out = self.dir / "manifest-v1.txt"
        result = self.run_build(out=out)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = out.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "FEXT1")
        emoji = lines[1].split("\t")
        self.assertEqual(emoji[0], "pack")
        self.assertEqual(emoji[1], "emoji-full")
        self.assertEqual(emoji[2], "emoji")
        self.assertEqual(emoji[3], "-")
        self.assertEqual(emoji[6], "1024")
        self.assertEqual(emoji[7], hashlib.sha256(b"\x00\x01\x00\x00" + b"e" * 1020).hexdigest())
        ext = lines[2].split("\t")
        self.assertEqual(ext[2], "fallback")
        self.assertEqual(ext[3], "CPYOSExt")
        self.assertEqual(ext[6], "512")

    def test_check_detects_stale_manifest(self):
        out = self.dir / "manifest-v1.txt"
        self.assertEqual(self.run_build(out=out).returncode, 0)
        self.assertEqual(self.run_build(out=out, check=True).returncode, 0)
        (self.dir / "Ext.ttf").write_bytes(b"y" * 512)
        self.assertNotEqual(self.run_build(out=out, check=True).returncode, 0)

    def test_rejects_duplicate_id_and_family(self):
        spec = json.loads(json.dumps(self.spec))
        spec["packs"][0]["id"] = "ext"
        spec["packs"][1]["id"] = "ext"
        result = self.run_build(spec=spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate id", result.stderr)

        spec = json.loads(json.dumps(self.spec))
        spec["packs"][0].update({"kind": "fallback", "family": "CPYOSExt"})
        spec["packs"][1].update({"kind": "fallback", "family": "CPYOSExt"})
        result = self.run_build(spec=spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate family", result.stderr)

    def test_rejects_unsafe_or_invalid_fields(self):
        cases = [
            ({"id": "bad id"}, "invalid id"),
            ({"family": "../x"}, "invalid family"),
            ({"file": "../Ext.ttf"}, "invalid file"),
            ({"file": "Ext.cpfont"}, ".ttf/.otf/.ttc"),
            ({"file": "..Ext.ttf"}, "invalid file"),
            ({"name": "Bad\x01Name"}, "invalid name"),
            ({"url": "http://e/Ext.ttf"}, "https"),
            ({"url": "https:///Ext.ttf"}, "https"),
            ({"kind": "other"}, "kind"),
        ]
        for index, (patch, needle) in enumerate(cases):
            spec = json.loads(json.dumps(self.spec))
            spec["packs"][1].update(patch)
            result = self.run_build(spec=spec)
            self.assertNotEqual(result.returncode, 0, f"case {index} should fail")
            self.assertIn(needle, result.stderr)

    def test_rejects_missing_or_tiny_asset(self):
        self.spec["packs"][1]["file"] = "missing.ttf"
        result = self.run_build(spec=self.spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing asset", result.stderr)

        self.spec["packs"][1]["file"] = "Ext.ttf"
        (self.dir / "Ext.ttf").write_bytes(b"x" * 10)
        result = self.run_build(spec=self.spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("out of range", result.stderr)

    def test_rejects_non_sfnt_asset(self):
        (self.dir / "Ext.ttf").write_bytes(b"x" * 512)
        result = self.run_build(spec=self.spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not an sfnt", result.stderr)

    def test_v2_adds_the_tier_column_and_defaults_to_none(self):
        spec = json.loads(json.dumps(self.spec))
        spec["packs"][1]["tier"] = "8105"
        out = self.dir / "manifest-v2.txt"
        result = self.run_build(spec=spec, out=out, fmt="v2")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = out.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "FEXT2")
        self.assertEqual(len(lines[1].split("\t")), 10)
        self.assertEqual(lines[2].split("\t")[-1], "8105")
        # A pack that claims nothing writes "-", it does not shrink the row.
        self.assertEqual(lines[1].split("\t")[-1], "-")

    def test_v1_refuses_to_drop_a_declared_tier(self):
        """The ordering footgun, made un-shippable.

        Publishing a manifest whose field the device cannot read would be silent; publishing
        v1 with the field quietly dropped is the same mistake in the other direction. FEXT2
        stays opt-in until firmware parses it, so this must be an error rather than a warning.
        """
        spec = json.loads(json.dumps(self.spec))
        spec["packs"][1]["tier"] = "8105"
        result = self.run_build(spec=spec)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--format v2", result.stderr)

    def test_v2_rejects_an_unusable_tier(self):
        for bad in ("8 105", "12345678", "", "8105;rm"):
            spec = json.loads(json.dumps(self.spec))
            spec["packs"][1]["tier"] = bad
            result = self.run_build(spec=spec, fmt="v2")
            self.assertNotEqual(result.returncode, 0, f"{bad!r} was accepted")
            self.assertIn("tier", result.stderr)

    def test_default_output_is_still_v1(self):
        # Nothing may start publishing v2 by accident; the flag is the only way.
        result = self.run_build(out=self.dir / "m.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.dir / "m.txt").read_text(encoding="utf-8").splitlines()[0], "FEXT1")

    def test_tier_rule_agrees_with_the_device_parser(self):
        """One rule, two languages -- checked structurally, because that is the drift class.

        The generator must not emit a value the firmware rejects (a manifest the device refuses
        reads as a silent "no updates"). So compare what each side actually allows: the alphabet
        and the length bound, read out of the sources instead of restated here.
        """
        src = (ROOT / "src" / "FontExtrasFormat.cpp").read_text()
        header = (ROOT / "src" / "FontExtrasFormat.h").read_text()
        body = src.split("bool isValidTier", 1)[1].split("\n}", 1)[0]
        self.assertIn("asciiAlphaNum(c)", body, "the C++ tier rule no longer allows alphanumerics")
        cpp_punct = set(re.findall(r"c == '(.)'", body))
        self.assertTrue(cpp_punct, "no explicit punctuation found in isValidTier")

        char_class = re.search(r"\[([^\]]+)\]", bfe.TIER_RE.pattern).group(1)
        py_punct = set(char_class.replace("A-Z", "").replace("a-z", "").replace("0-9", "").replace("\\.", "."))
        self.assertEqual(cpp_punct, py_punct, f"alphabet drift: {cpp_punct} vs {py_punct}")

        # ... and the comparison has to discriminate. Without this loop the assertion above is
        # only as good as the day it was written: if one side later loses a character, the set
        # equality is the whole defence, so prove it fires by removing each one in memory.
        for needle in (" || c == '_'", " || c == '.'", " || c == '-'"):
            self.assertEqual(body.count(needle), 1, f"expected exactly one {needle} in isValidTier")
            dropped = set(re.findall(r"c == '(.)'", body.replace(needle, "")))
            self.assertNotEqual(dropped, py_punct, f"dropping {needle} went unnoticed")

        max_bytes = int(re.search(r"kMaxTierBytes = (\d+)", header).group(1))
        bound = re.search(r"\{1,(\d+)\}", bfe.TIER_RE.pattern)
        self.assertIsNotNone(bound, "the python tier rule lost its length bound")
        self.assertEqual(int(bound.group(1)), max_bytes - 1,
                         "python allows a length the device's char[] cannot hold")


if __name__ == "__main__":
    unittest.main()
