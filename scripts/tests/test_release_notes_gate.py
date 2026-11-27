#!/usr/bin/env python3
"""Gate for the pre-build OTA-note check (scripts/check_release_notes.py).

0.8.4's first release run passed the 20-minute build and then died because one
English note contained '<' (the note format forbids shell/HTML metacharacters).
The check moved in front of the build; these cases pin that it rejects the same
things the packager does, and that a good file passes.

Run: python3 -m unittest scripts.tests.test_release_notes_gate -v
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_release_notes.py"

GOOD = {
    "en": ["Plus packs use target-scoped names now", "Firmware behaviour is unchanged"],
    "zh": ["Plus 数据包改为目标前缀", "固件行为不变"],
}


def run(version: str, root: pathlib.Path) -> subprocess.CompletedProcess:
    """Run the checker with its repo root redirected at a fixture tree."""
    code = (
        "import pathlib, sys\n"
        f"sys.path.insert(0, {str(REPO / 'scripts')!r})\n"
        "import check_release_notes as check\n"
        f"check.ROOT = pathlib.Path({str(root)!r})\n"
        f"sys.exit(check.main(['check', {version!r}]))\n"
    )
    return subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)


class ReleaseNotesGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "docs" / "releases").mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, version: str, payload: dict) -> None:
        path = self.root / "docs" / "releases" / f"{version}.ota.json"
        path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

    def test_valid_notes_pass(self):
        self.write("9.9.9", GOOD)
        result = run("9.9.9", self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 en, 2 zh", result.stdout)

    def test_metacharacter_is_rejected_before_the_build(self):
        """The exact 0.8.4 failure: '<' in an English note."""
        payload = json.loads(json.dumps(GOOD))
        payload["en"][0] = "Packs are now published as plus-<name>"
        self.write("9.9.9", payload)
        result = run("9.9.9", self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("unsafe en release note", result.stderr)

    def test_overlong_note_and_missing_file_are_rejected(self):
        payload = json.loads(json.dumps(GOOD))
        payload["en"][0] = "x" * 97
        self.write("9.9.9", payload)
        self.assertEqual(run("9.9.9", self.root).returncode, 1)
        result = run("0.0.0", self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing", result.stderr)

    def test_the_shipping_notes_validate(self):
        """Whatever version platformio.ini names must have a valid notes file.

        Skipped in a development tree: the OTA notes for a released version live
        in the release tree, and dev's version number is not a release.
        """
        ini = (REPO / "platformio.ini").read_text(encoding="utf-8")
        version = next(line.split("=", 1)[1].strip() for line in ini.splitlines()
                       if line.startswith("version ="))
        if not (REPO / "docs" / "releases" / f"{version}.ota.json").is_file():
            self.skipTest(f"docs/releases/{version}.ota.json is release-tree only")
        result = subprocess.run([sys.executable, str(SCRIPT), version],
                                capture_output=True, text=True, cwd=REPO)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
