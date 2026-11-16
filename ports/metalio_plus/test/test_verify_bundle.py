#!/usr/bin/env python3
"""Integration tests for scripts/verify_metalio_bundle.py using synthetic zips."""

import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "verify_metalio_bundle.py"

LEGAL = (
    "legal/LICENSE",
    "legal/NOTICE-JJWXC.md",
    "legal/THIRD_PARTY_NOTICES.md",
    "legal/MIT.txt",
    "legal/Apache-2.0.txt",
    "legal/OFL-1.1.txt",
    "legal/AGPL-3.0.txt",
    "legal/Metalio-E-INK4-Plus-MIT.txt",
    "legal/Crosspoint-Simulator-MIT.txt",
)
NOTICE = ("CPYOS — UNTESTED ON HARDWARE\npinyin 0xCD0000 web 0xD26000 hyphen 0xD60000 "
          "language 0xDD0000 emoji 0xE30000 core 0xE50000 bringup_serial\n"
          "bringup_serial.py\n")
SBOM = "espressif/esp_tinyusb 2.4.0\nespressif/tinyusb 0.21.0~4\nUNTESTED ON HARDWARE\n"


def image(project="cpyos_metalio_plus", version="0.0.0-untested"):
    data = bytearray(0x100)
    struct.pack_into("<I", data, 0x20, 0xABCD5432)
    data[0x30:0x50] = version.encode().ljust(0x20, b"\0")
    data[0x50:0x70] = project.encode().ljust(0x20, b"\0")
    return bytes(data)


def make_bundle(path: Path):
    members = {}
    for variant in ("bsp", "bringup", "reader", "shell"):
        members[f"cpyos_metalio_plus_{variant}_20261106_untested.bin"] = image()
    members["cpyos_metalio_plus_pinyin_untested.dat"] = b"PYDB" + b"\0" * 60
    members["cpyos_metalio_plus_hyphen_untested.dat"] = b"HYPN" + b"\0" * 60
    members["cpyos_metalio_plus_i18n_pack_20261106_untested.dat"] = b"JILP" + b"\0" * 60
    ttf = b"\x00" * 16
    members["cpyos_metalio_plus_emoji_pack_20261106_untested.dat"] = (
        struct.pack("<4sB3xII16x", b"EMJ1", 1, len(ttf), zlib.crc32(ttf) & 0xFFFFFFFF) + ttf)
    for name in LEGAL:
        members[name] = b"text\n"
    sums = "".join(f"{hashlib.sha256(v).hexdigest()}  {k}\n" for k, v in members.items())
    with zipfile.ZipFile(path, "w") as bundle:
        for name, data in members.items():
            bundle.writestr(name, data)
        bundle.writestr("SHA256SUMS", sums)
        bundle.writestr("SBOM.txt", SBOM)
        bundle.writestr("UNTESTED-ON-HARDWARE.txt", NOTICE)
    return members


class VerifyBundleTest(unittest.TestCase):
    def run_verifier(self, bundle: Path):
        return subprocess.run([sys.executable, str(SCRIPT), "--bundle", str(bundle)],
                              capture_output=True, text=True)

    def test_valid_bundle_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bundle.zip"
            make_bundle(path)
            result = self.run_verifier(path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("bundle OK", result.stdout)

    def test_wrong_project_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bundle.zip"
            make_bundle(path)
            with zipfile.ZipFile(path, "w") as bundle:
                members = make_bundle(Path(tmp) / "ok.zip")
                members["cpyos_metalio_plus_shell_20261106_untested.bin"] = image(project="crossmux")
                sums = "".join(f"{hashlib.sha256(v).hexdigest()}  {k}\n" for k, v in members.items())
                for name, data in members.items():
                    bundle.writestr(name, data)
                bundle.writestr("SHA256SUMS", sums)
                bundle.writestr("SBOM.txt", SBOM)
                bundle.writestr("UNTESTED-ON-HARDWARE.txt", NOTICE)
            result = self.run_verifier(path)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cpyos_metalio_plus", result.stderr)

    def test_digest_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bundle.zip"
            members = make_bundle(path)
            with zipfile.ZipFile(path, "w") as bundle:
                for name, data in members.items():
                    bundle.writestr(name, data)
                sums = "".join(f"{'0' * 64}  {k}\n" for k in members)
                bundle.writestr("SHA256SUMS", sums)
                bundle.writestr("SBOM.txt", SBOM)
                bundle.writestr("UNTESTED-ON-HARDWARE.txt", NOTICE)
            result = self.run_verifier(path)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("digest mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
