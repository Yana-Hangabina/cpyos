#!/usr/bin/env python3
"""Gate tests for publishing a staged PDF engine (verify_release_artifacts.check_pdf_engine).

The engine is a second artifact with its own compatibility class.  A stale one is
the same size as the good one and downloads happily; only the device refuses it
afterwards.  The gate therefore checks the rolling PDFENG2 index, the blob header,
and that the blob was linked from the firmware image being published.

Every test asserts an OUTCOME, and the negative paths are checked as carefully as
the positive one -- a gate that cannot fail is not a gate.
"""
from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_release_artifacts",
                                              ROOT / "scripts" / "verify_release_artifacts.py")
vra = importlib.util.module_from_spec(SPEC)
sys.argv = ["verify_release_artifacts"]
SPEC.loader.exec_module(vra)

SIZE = 1024


def good_abi() -> int:
    return vra._header_constant("kPdfModuleAbiVersion")


def good_host() -> int:
    return (vra._header_constant("kPdfModuleHostApiMajor") << 16) | \
        vra._header_constant("kPdfModuleHostApiMinor")


def good_layout() -> int:
    import re
    keys = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;",
                      (ROOT / "src/pdf/PdfModuleApi.h").read_text())
    return int(keys[1], 16)  # flash mapping (readpico)


def engine_bytes(build_id: bytes, abi: int | None = None, host_api: int | None = None,
                 layout: int | None = None, rev: int = 7,
                 magic: int = vra.PDF_MODULE_MAGIC, version: int = 2) -> bytes:
    abi = good_abi() if abi is None else abi
    host_api = good_host() if host_api is None else host_api
    layout = good_layout() if layout is None else layout
    head = bytearray(struct.pack("<IHHII", magic, version, abi, host_api, layout))
    # headerBytes, codeOffset, entryOffset, codeSize, codeCrc, rodataOff, rodataSize,
    # rodataCrc, dataOff, dataSize, dataCrc, bss, engineRev
    head += struct.pack("<13I", 0x10000, 0x10000, 0x40, 0x200, 0, 0x20000, 0x100, 0,
                        0x30000, 0x100, 0, 0x100, rev)
    head += build_id
    head += b"\0" * (SIZE - len(head))
    return bytes(head) + b"\xAA" * (SIZE - len(head))


def firmware_bytes(build_id: bytes) -> bytes:
    image = bytearray(0x200)
    image[vra.ELF_SHA256_OFFSET:vra.ELF_SHA256_OFFSET + 32] = build_id
    return bytes(image)


class TestPdfEnginePublishGate(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "site" / "pdf-engine").mkdir(parents=True)
        self.firmware = Path(self._tmp.name) / "firmware.bin"
        self.build = hashlib.sha256(b"one specific ELF").digest()
        self.firmware.write_bytes(firmware_bytes(self.build))
        self.engine = engine_bytes(self.build)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def stage(self, blob: bytes | None = None, size: int | None = None, sha: str | None = None,
              build: str | None = None, rev: int | None = None, abi: int | None = None,
              host_api: int | None = None, layout: int | None = None,
              name: str = "pdfmod.bin",
              url: str = "https://github.com/Yana-Hangabina/cpyos/releases/download/pdf-engine/pdfmod.bin",
              header: str = "PDFENG2") -> None:
        blob = self.engine if blob is None else blob
        (self.root / "site" / "pdf-engine" / name).write_bytes(blob)
        line = "\t".join([url, str(len(blob) if size is None else size),
                          sha or hashlib.sha256(blob).hexdigest(), str(good_abi() if abi is None else abi),
                          str((good_host() >> 16) if host_api is None else (host_api >> 16)),
                          str((good_host() & 0xFFFF) if host_api is None else (host_api & 0xFFFF)),
                          "0x%08X" % (good_layout() if layout is None else layout),
                          str(7 if rev is None else rev),
                          build or self.build.hex()])
        (self.root / "site" / "pdf-engine" / "manifest-v2.txt").write_text(header + "\n" + line + "\n")

    def manifest(self) -> Path:
        return self.root / "site" / "pdf-engine" / "manifest-v2.txt"

    def run_gate(self, staged_required: bool = True):
        return vra.check_pdf_engine(self.root, str(self.firmware), staged_required)

    def test_paired_engine_is_accepted(self) -> None:
        self.stage()
        notes = self.run_gate()
        self.assertEqual(len(notes), 1)
        self.assertIn("matches this firmware", notes[0])
        self.assertIn(f"ABI {good_abi()}", notes[0])

    def test_missing_staging_is_not_an_error_when_nothing_is_published(self) -> None:
        notes = self.run_gate(staged_required=False)
        self.assertIn("not staged", notes[0])

    def test_staged_engine_without_a_firmware_flag_is_refused(self) -> None:
        self.stage()
        with self.assertRaises(SystemExit):
            self.run_gate(staged_required=False)

    def test_stale_build_id_is_refused(self) -> None:
        blob = engine_bytes(b"\x11" * 32)
        self.stage(blob=blob, build="11" * 32)
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_manifest_build_id_must_match_the_blob(self) -> None:
        self.stage(build="22" * 32)
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_wrong_abi_host_or_layout_is_not_selected(self) -> None:
        # Changing any class field makes the index carry no entry for this app, which is a
        # refusal rather than a download of an image the loader would reject.
        for kwargs in ({"abi": good_abi() - 1}, {"host_api": (good_host() >> 16) << 16 | 0xFFFF},
                       {"layout": good_layout() ^ 1}):
            if kwargs.get("host_api") is not None and (kwargs["host_api"] & 0xFFFF) <= (good_host() & 0xFFFF):
                kwargs["host_api"] = ((good_host() >> 16) + 1) << 16
            with self.subTest(**{k: hex(v) for k, v in kwargs.items()}):
                self.stage(**kwargs)
                with self.assertRaises(SystemExit):
                    self.run_gate()

    def test_blob_header_must_match_the_manifest_entry(self) -> None:
        blob = engine_bytes(self.build, abi=good_abi() - 1)
        self.stage()  # manifest says the good ABI, blob disagrees
        (self.root / "site" / "pdf-engine" / "pdfmod.bin").write_bytes(blob)
        self.stage(size=len(blob), sha=hashlib.sha256(blob).hexdigest())
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_engine_rev_must_match_the_blob(self) -> None:
        blob = engine_bytes(self.build, rev=9)
        self.stage(blob=blob, rev=7)
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_size_and_sha_must_match_the_staged_blob(self) -> None:
        self.stage(size=len(self.engine) - 1)
        with self.assertRaises(SystemExit):
            self.run_gate()
        self.stage(sha="f" * 64)
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_manifest_url_must_live_where_the_firmware_looks(self) -> None:
        if not vra._platformio_pdf_manifest_url():
            self.skipTest("platformio.ini defines no engine manifest URL")
        self.stage(url="https://example.org/elsewhere/pdfmod.bin")
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_not_an_engine_image_is_refused(self) -> None:
        blob = engine_bytes(self.build, magic=0xDEADBEEF)
        self.stage(blob=blob, sha=hashlib.sha256(blob).hexdigest(), size=len(blob))
        with self.assertRaises(SystemExit):
            self.run_gate()

    def test_malformed_manifests_are_refused(self) -> None:
        cases = [
            "PDFENG1\n" + "\t".join(["https://x/y", "1024", "a" * 64, "4", "1", "0", "0x1", "1", "b" * 64]),
            "PDFENG2\nonly\ttwo\tfields\n",
            "PDFENG2\n" + "\t".join(["http://x/y", "1024", "a" * 64, "4", "1", "0", "0x1", "1", "b" * 64]),
        ]
        (self.root / "site" / "pdf-engine" / "pdfmod.bin").write_bytes(self.engine)
        for text in cases:
            self.manifest().write_text(text + "\n")
            with self.assertRaises(SystemExit):
                self.run_gate()


if __name__ == "__main__":
    unittest.main(verbosity=2)
