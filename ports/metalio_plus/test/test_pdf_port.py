"""Host regressions for the Plus PDF port's artifacts and staging tools.

No device and no MuPDF build are required: the engine manifest and the staging
script are pure file operations, so a synthetic ELF/blob pair pins the pairing
rule (appTag == sha256(ELF)) that the device's 0xb0 comparison relies on.
"""
from __future__ import annotations

import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


class StagingTest(unittest.TestCase):
    def test_sha_only_matches_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            engine = tmpdir / "pdfmod-rv.bin"
            bundle = tmpdir / "pdf_full_assets.bin"
            engine.write_bytes(b"engine-bytes" * 100)
            bundle.write_bytes(b"bundle-bytes" * 100)
            out = subprocess.check_output(
                [
                    sys.executable,
                    str(ROOT / "ports/metalio_plus/scripts/stage_pdf_sd.py"),
                    "--engine",
                    str(engine),
                    "--bundle",
                    str(bundle),
                    "--sha-only",
                ],
                text=True,
            )
            self.assertIn(sha256(engine), out)
            self.assertIn(sha256(bundle), out)

    def test_copies_to_card_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            card = tmpdir / "card"
            card.mkdir()
            engine = tmpdir / "pdfmod-rv.bin"
            bundle = tmpdir / "pdf_full_assets.bin"
            engine.write_bytes(b"E" * 4096)
            bundle.write_bytes(b"B" * 8192)
            subprocess.check_call(
                [
                    sys.executable,
                    str(ROOT / "ports/metalio_plus/scripts/stage_pdf_sd.py"),
                    "--card",
                    str(card),
                    "--engine",
                    str(engine),
                    "--bundle",
                    str(bundle),
                ],
                stdout=subprocess.DEVNULL,
            )
            self.assertEqual((card / ".crosspoint/pdf/pdfmod.bin").read_bytes(), engine.read_bytes())
            self.assertEqual((card / ".crosspoint/pdf_assets.bin").read_bytes(), bundle.read_bytes())


class EngineManifestTest(unittest.TestCase):
    def test_manifest_binds_to_elf_sha(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            elf = tmpdir / "app.elf"
            blob = tmpdir / "pdfmod.bin"
            out = tmpdir / "manifest.txt"
            elf.write_bytes(b"\x7fELF" + os.urandom(2048))
            # The manifest builder refuses a blob whose v2 header does not carry
            # this ELF's build id (bytes 68..100) and the tree's compatibility class.
            build = hashlib.sha256(elf.read_bytes()).digest()
            blob_data = bytearray(os.urandom(4096))
            blob_data[0:16] = struct.pack("<IHHII", 0x31504D43, 2, 4, 0x00010000, 0xEE4B634F)
            blob_data[16:68] = struct.pack("<13I", 0x10000, 0x10000, 0x40, 0x200, 0, 0x20000, 0x100,
                                           0, 0x30000, 0x100, 0, 0x100, 12)
            blob_data[68:100] = build
            blob.write_bytes(bytes(blob_data))
            subprocess.check_call(
                [
                    sys.executable,
                    str(ROOT / "scripts/build_pdf_engine_manifest.py"),
                    "--firmware-elf",
                    str(elf),
                    "--pdfmod",
                    str(blob),
                    "--url",
                    "https://example.invalid/pdfmod.bin",
                    "--out",
                    str(out),
                ],
                stdout=subprocess.DEVNULL,
            )
            lines = out.read_text().strip().splitlines()
            self.assertEqual(lines[0], "PDFENG2")
            fields = lines[1].split("\t")
            self.assertEqual(len(fields), 9)
            self.assertEqual(fields[0], "https://example.invalid/pdfmod.bin")
            self.assertEqual(int(fields[1]), blob.stat().st_size)
            self.assertEqual(fields[2], sha256(blob))
            self.assertEqual((fields[3], fields[4], fields[5]), ("4", "1", "0"))
            self.assertEqual(fields[6], "0xEE4B634F")
            self.assertEqual(int(fields[7]), 12)
            self.assertEqual(fields[8], sha256(elf))


class BuiltArtifactsTest(unittest.TestCase):
    """Skip when the RISC-V build outputs are absent (clean checkout / CI)."""

    def test_module_header_tags_the_firmware_elf(self) -> None:
        elf = ROOT / "ports/metalio_plus/build_shell/cpyos_metalio_plus.elf"
        module = ROOT / "build/pdfmod-rv.bin"
        if not elf.is_file() or not module.is_file():
            self.skipTest("build artifacts absent")
        data = module.read_bytes()
        self.assertGreater(len(data), 128)
        magic, version, abi, host_api, layout = struct.unpack_from("<IHHII", data, 0)
        self.assertEqual(magic, 0x31504D43)
        self.assertEqual((version, abi), (2, 4))
        self.assertEqual(host_api, 0x00010000)
        self.assertEqual(layout, 0xEE4B634F)  # Plus SD/PSRAM mapping
        self.assertEqual(data[68:100], hashlib.sha256(elf.read_bytes()).digest())
        # The CRC fields must describe the images the loader will verify.
        (code_size,) = struct.unpack_from("<I", data, 28)
        (code_crc,) = struct.unpack_from("<I", data, 32)
        self.assertEqual(data[0x10000:0x10000 + code_size].__len__(), code_size)
        import zlib

        self.assertEqual(zlib.crc32(data[0x10000:0x10000 + code_size]) & 0xFFFFFFFF, code_crc)

    def test_asset_bundle_header(self) -> None:
        bundle = ROOT / "build/pdf_full_assets.bin"
        if not bundle.is_file():
            self.skipTest("asset bundle absent")
        data = bundle.read_bytes()
        self.assertEqual(data[:4], b"PFPK")
        (font_raw, font_comp, cmap_size) = struct.unpack_from("<III", data, 8)
        self.assertGreater(font_raw, 0)
        self.assertGreater(font_comp, 0)
        self.assertGreater(cmap_size, 0)
        self.assertEqual(len(data) - 20, font_comp + cmap_size)


if __name__ == "__main__":
    unittest.main()
