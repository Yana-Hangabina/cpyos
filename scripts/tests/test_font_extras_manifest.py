import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build_font_extras_manifest.py"


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

    def run_build(self, spec=None, out=None, check=False):
        spec_path = self.dir / "packs.json"
        spec_path.write_text(json.dumps(spec if spec is not None else self.spec), encoding="utf-8")
        cmd = [sys.executable, str(SCRIPT), "--spec", str(spec_path), "--assets-dir", str(self.dir)]
        if out is not None:
            cmd += ["--out", str(out)]
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


if __name__ == "__main__":
    unittest.main()
