#!/usr/bin/env python3
"""Host tests for the pdf-engine gate in scripts/build_cpyos_site.py.

`site/pdf-engine/` is the mechanism behind "update the PDF engine without
re-flashing": the device reads the PDFENG2 index, picks the newest entry for its
compatibility class, downloads the blob and checks size + sha256.  A fresh index
beside a stale blob (or a missing file) used to reach production and make
检查更新 fail on every device.

The gate also checks that the staged engine was linked from the firmware image
published in the same release (build id), because that is a packaging mistake the
device can only discover after the download.
"""
from __future__ import annotations

import hashlib
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build_cpyos_site import check_pdf_engine, firmware_is_xip_build  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
BUILD = "a" * 64


def _constant(name: str, text: str) -> int:
    match = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", text)
    if not match:
        raise RuntimeError(name)
    return int(match.group(1), 0)


API = (REPO / "src/pdf/PdfModuleApi.h").read_text()
HOST = (REPO / "src/pdf/PdfModuleHost.h").read_text()
ABI = _constant("kPdfModuleAbiVersion", API)
MAJOR = _constant("kPdfModuleHostApiMajor", HOST)
MINOR = _constant("kPdfModuleHostApiMinor", HOST)
LAYOUT = int(re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", API)[1], 16)
REV = 7


def module_blob(build: str = BUILD, rev: int = REV, abi: int = ABI,
                layout: int = LAYOUT, body: bytes = b"engine-bytes") -> bytes:
    import struct
    head = bytearray(struct.pack("<IHHII", 0x31504D43, 2, abi, (MAJOR << 16) | MINOR, layout))
    head += struct.pack("<13I", 0x10000, 0x10000, 0x40, 0x200, 0, 0x20000, 0x100, 0,
                        0x30000, 0x100, 0, 0x100, rev)
    head += bytes.fromhex(build)
    return bytes(head) + body


def firmware_image(build: str = BUILD) -> bytes:
    head = bytearray(0xB0)
    return bytes(head) + bytes.fromhex(build) + b"\0" * 16


class PdfEngineSiteGate(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile
        self._tmp = Path(tempfile.mkdtemp())
        self.site = self._tmp / "site"
        self.engine = self.site / "pdf-engine"
        self.engine.mkdir(parents=True)
        self.fw = self._tmp / "readpico-firmware.bin"
        self.fw.write_bytes(firmware_image())

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def publish(self, blob: bytes | None = None, size: int | None = None, sha: str | None = None,
                name: str = "pdfmod.bin", build: str = BUILD, rev: int = REV,
                abi: int = ABI, layout: int = LAYOUT) -> None:
        blob = module_blob(build, rev, abi, layout) if blob is None else blob
        (self.engine / name).write_bytes(blob)
        url = f"https://example.invalid/cpyos/pdf-engine/{name}"
        digest = sha or hashlib.sha256(blob).hexdigest()
        line = "\t".join([url, str(size if size is not None else len(blob)), digest, str(abi),
                          str(MAJOR), str(MINOR), f"0x{layout:08X}", str(rev), build])
        (self.engine / "manifest-v2.txt").write_text("PDFENG2\n" + line + "\n", encoding="utf-8")

    def refused(self, site: Path, firmware: Path | None = None) -> str:
        import contextlib, io
        firmware = self.fw if firmware is None else firmware
        buf = io.StringIO()
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(buf):
                check_pdf_engine(site, firmware)
        return buf.getvalue()

    def test_absent_directory_is_not_an_error(self):
        check_pdf_engine(self._tmp / "no-such-site")

    def test_xip_firmware_without_an_engine_is_refused(self):
        fw = self._tmp / "xip-firmware.bin"
        fw.write_bytes(firmware_image() + b"\0refusing the XIP engine\0")
        self.assertIn("does not exist", self.refused(self._tmp / "no-such-site", fw))

    def test_plain_firmware_needs_no_engine_directory(self):
        fw = self._tmp / "plain-firmware.bin"
        fw.write_bytes(firmware_image())
        self.assertFalse(firmware_is_xip_build(fw))
        check_pdf_engine(self._tmp / "no-such-site", fw)

    def test_translation_text_is_not_an_xip_marker(self):
        yaml = (REPO / "lib" / "I18n" / "translations" / "english.yaml").read_text(encoding="utf-8")
        match = re.search(r'^STR_PDF_ENGINE_INSTALL:\s*"([^"]+)"', yaml, re.M)
        self.assertIsNotNone(match, "the label this test guards is gone from the translations")
        label = match.group(1).encode("utf-8")
        self.assertIn(b"/pdfmod.bin", label)
        normal = self._tmp / "normal-firmware.bin"
        normal.write_bytes(firmware_image() + b"\0" + label + b"\0")
        self.assertFalse(firmware_is_xip_build(normal))
        check_pdf_engine(self._tmp / "no-such-site", normal)

    def test_xip_marker_actually_exists_in_a_real_image(self):
        real = REPO / ".pio" / "build" / "readpico_pdf" / "firmware.bin"
        if not real.is_file():
            self.skipTest("build -e readpico_pdf first")
        self.assertTrue(firmware_is_xip_build(real))

    def test_consistent_pair_passes(self):
        self.publish()
        check_pdf_engine(self.site, self.fw)

    def test_missing_app_image_is_refused(self):
        self.publish()
        self.assertIn('cannot verify the engine/app build pairing',
                      self.refused(self.site, self._tmp / 'absent.bin'))

    def test_engine_built_for_another_app_is_refused(self):
        self.publish(build="b" * 64)
        self.assertIn("linked from another ELF", self.refused(self.site))

    def test_index_must_carry_this_firmwares_class(self):
        self.publish(abi=ABI - 1)
        self.assertIn("no entry for this firmware", self.refused(self.site))
        self.publish(layout=LAYOUT ^ 1)
        self.assertIn("no entry for this firmware", self.refused(self.site))

    def test_stale_blob_is_refused(self):
        self.publish()
        path = self.engine / "pdfmod.bin"
        path.write_bytes(b"\0" * path.stat().st_size)
        self.assertIn("sha256", self.refused(self.site))

    def test_manifest_rev_must_match_the_blob(self):
        self.publish()
        text = (self.engine / "manifest-v2.txt").read_text(encoding="utf-8").split("\t")
        text[7] = str(REV + 1)  # manifest claims a newer revision than the blob header
        (self.engine / "manifest-v2.txt").write_text("\t".join(text), encoding="utf-8")
        self.assertIn("carries rev", self.refused(self.site))

    def test_manifest_build_id_must_match_the_blob(self):
        blob = module_blob()
        self.publish(blob=blob)
        text = (self.engine / "manifest-v2.txt").read_text(encoding="utf-8")
        (self.engine / "manifest-v2.txt").write_text(text.replace(BUILD, "c" * 64), encoding="utf-8")
        self.assertIn("build id does not match", self.refused(self.site))

    def test_wrong_sha_is_refused(self):
        self.publish(sha="f" * 64)
        self.assertIn("sha256", self.refused(self.site))

    def test_wrong_size_is_refused(self):
        self.publish(size=7)
        self.assertIn("is 7 B", self.refused(self.site))

    def test_missing_manifest_is_refused(self):
        self.publish()
        (self.engine / "manifest-v2.txt").unlink()
        self.assertIn("without manifest-v2.txt", self.refused(self.site))

    def test_missing_blob_is_refused(self):
        self.publish()
        (self.engine / "pdfmod.bin").unlink()
        self.assertIn("which is not in", self.refused(self.site))

    def test_malformed_line_is_refused(self):
        (self.engine / "manifest-v2.txt").write_text("PDFENG2\njust-a-url\n", encoding="utf-8")
        (self.engine / "pdfmod.bin").write_bytes(b"x")
        self.assertIn("expected 9 TAB-separated fields", self.refused(self.site))

    def test_missing_header_is_refused(self):
        self.publish()
        text = (self.engine / "manifest-v2.txt").read_text(encoding="utf-8")
        (self.engine / "manifest-v2.txt").write_text(text.replace("PDFENG2\n", ""), encoding="utf-8")
        self.assertIn("is not a PDFENG2 index", self.refused(self.site))


if __name__ == "__main__":
    unittest.main()
