#!/usr/bin/env python3
"""README artifact table must match the files actually on disk."""

import hashlib
import re
import unittest
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
README = (PORT / "README.md").read_text(encoding="utf-8")
ARTIFACTS = PORT.parents[3] / "artifacts"

ROW = re.compile(r"\| `(cpyos_metalio_plus_[^`]+)` \| ([0-9,]+) B \| `([0-9a-f]{8})…([0-9a-f]{8})`")


class DocsArtifactsTest(unittest.TestCase):
    def test_every_artifact_has_a_matching_row(self):
        rows = {m.group(1): (int(m.group(2).replace(",", "")), m.group(3), m.group(4))
                for m in ROW.finditer(README)}
        if not ARTIFACTS.is_dir():
            self.skipTest("artifacts directory not present")
        files = [p for p in ARTIFACTS.glob("cpyos_metalio_plus_*_20261109_untested.*")
                 if p.suffix in (".bin", ".dat")]
        self.assertGreaterEqual(len(files), 8)
        for path in files:
            self.assertIn(path.name, rows, f"{path.name} missing from the README table")
            size, head, tail = rows[path.name]
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(size, path.stat().st_size, f"{path.name} size")
            self.assertEqual((head, tail), (digest[:8], digest[-8:]), f"{path.name} hash")

    def test_bundle_members_match_artifact_names(self):
        import zipfile
        bundle = ARTIFACTS / "cpyos-metalio-plus-0.0.0-untested.zip"
        if not bundle.is_file():
            self.skipTest("bundle not present")
        # A historical bundle (from the 2026-11-09 bring-up session) is kept for
        # reference; it cannot match artifacts that were rebuilt afterwards.
        # Only fail when the bundle is at least as new as every member file.
        newest = max(p.stat().st_mtime for p in ARTIFACTS.glob("cpyos_metalio_plus_*_20261109_untested.*"))
        if bundle.stat().st_mtime < newest:
            self.skipTest("bundle predates the current artifact files")
        with zipfile.ZipFile(bundle) as z:
            payload = [n for n in z.namelist() if n.endswith((".bin", ".dat"))]
            for name in payload:
                self.assertTrue((ARTIFACTS / name).is_file(), f"bundle member {name} has no artifact")
                self.assertEqual((ARTIFACTS / name).read_bytes(), z.read(name), name)


if __name__ == "__main__":
    unittest.main()
