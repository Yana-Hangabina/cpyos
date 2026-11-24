"""Host regressions for ports/metalio_plus/scripts/flash_plus_data.py.

The script only writes manifest rows that stay inside the data partition and
match their recorded sha256; a silent bad flash is the failure mode this pins.
Absolute blob paths keep the test independent of the repository checkout.
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "ports/metalio_plus/scripts/flash_plus_data.py"
BASE = 0xCD0000
SIZE = 0x320000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_manifest(tmp: Path, rows: list[tuple[str, int, bytes, str, Path | None]]) -> Path:
    lines = [
        "# test manifest",
        "partition\tspiffs\t0x%08x\t0x%08x\t\t\t" % (BASE, SIZE),
        "reserved\t0x00080000\t12\tfactory bytes only",
    ]
    for name, offset, blob, sha, blob_path in rows:
        path = blob_path if blob_path is not None else tmp / (name + ".dat")
        if blob_path is None:
            path.write_bytes(blob)
        lines.append("data\t%s\t0x%08x\t0x%08x\t%d\t%s\t%s"
                     % (name, offset, BASE + offset, len(blob), sha, path))
    manifest = tmp / "manifest.txt"
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return manifest


def run(manifest: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--manifest", str(manifest), "--dry-run"],
                          capture_output=True, text=True)


class FlashPlusDataTest(unittest.TestCase):
    def test_valid_manifest_reports_every_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            blob = b"pin" * 100
            manifest = build_manifest(tmpdir, [("pinyin", 0, blob, sha256(blob), None)])
            result = run(manifest)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("pinyin@0xcd0000", result.stdout)

    def test_sha_mismatch_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            blob = b"payload"
            manifest = build_manifest(tmpdir, [("core", 0x180000, blob, "0" * 64, None)])
            result = run(manifest)
            self.assertEqual(result.returncode, 1)
            self.assertIn("sha256 differs", result.stderr)

    def test_row_outside_partition_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            blob = b"x" * 16
            manifest = build_manifest(tmpdir, [("core", SIZE - 8, blob, sha256(blob), None)])
            result = run(manifest)
            self.assertEqual(result.returncode, 1)
            self.assertIn("outside the data partition", result.stderr)

    def test_missing_blob_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            blob = b"gone"
            missing = tmpdir / "missing.dat"
            manifest = build_manifest(tmpdir, [("web", 0x56000, blob, sha256(blob), missing)])
            result = run(manifest)
            self.assertEqual(result.returncode, 1)
            self.assertIn("missing", result.stderr)


if __name__ == "__main__":
    unittest.main()
