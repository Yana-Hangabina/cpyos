#!/usr/bin/env python3
"""Host tests for the Plus packaging gates (pack <-> firmware key match, label)."""

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "package_metalio_plus.py"
spec = importlib.util.spec_from_file_location("package_plus", SCRIPT)
pkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pkg)


def make_keys_header(path: Path, count=1789, signature=0xAA8C4A46, content=0x81D37B8C):
    path.write_text(
        f"constexpr uint32_t I18N_KEY_COUNT = {count};\n"
        f"constexpr uint32_t I18N_KEY_SIGNATURE = 0x{signature:08X}u;\n"
        f"constexpr uint32_t I18N_CONTENT_SIGNATURE = 0x{content:08X}u;\n",
        encoding="utf-8")


def make_pack(path: Path, count=1789, signature=0xAA8C4A46, content=0x81D37B8C, languages=33,
              total=None):
    data = bytearray(128)
    data[0:4] = b"JILP"
    struct.pack_into("<I", data, 8, count)
    struct.pack_into("<I", data, 12, signature)
    struct.pack_into("<H", data, 16, languages)
    struct.pack_into("<I", data, 32, len(data) if total is None else total)
    struct.pack_into("<I", data, 36, content)
    path.write_bytes(data)


class PackGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.keys = self.dir / "I18nKeys.h"
        self.pack = self.dir / "i18n_pack.dat"
        make_keys_header(self.keys)

    def tearDown(self):
        self.tmp.cleanup()

    def test_matching_pack_is_accepted(self):
        make_pack(self.pack)
        summary = pkg.check_pack(self.pack, self.keys)
        self.assertIn("1789 keys", summary)
        self.assertIn("33 languages", summary)

    def test_key_count_mismatch_is_refused(self):
        make_pack(self.pack, count=1600)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_pack(self.pack, self.keys)
        self.assertIn("keyCount", str(ctx.exception))

    def test_key_signature_mismatch_is_refused(self):
        make_pack(self.pack, signature=0x12345678)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_pack(self.pack, self.keys)
        self.assertIn("keySignature", str(ctx.exception))

    def test_content_signature_mismatch_is_refused(self):
        make_pack(self.pack, content=0xDEADBEEF)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_pack(self.pack, self.keys)
        self.assertIn("contentSignature", str(ctx.exception))

    def test_truncated_pack_is_refused(self):
        make_pack(self.pack, total=999)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_pack(self.pack, self.keys)
        self.assertIn("totalSize", str(ctx.exception))

    def test_bad_magic_is_refused(self):
        self.pack.write_bytes(b"NOPE" + b"\0" * 124)
        with self.assertRaises(SystemExit):
            pkg.check_pack(self.pack, self.keys)


def make_emoji_pack(path: Path, ttf: bytes = b"\0" * 1024, version: int = 1, crc=None, size=None):
    import zlib
    header = struct.pack("<4sB3xII16x", b"EMJ1", version, len(ttf) if size is None else size,
                         zlib.crc32(ttf) & 0xFFFFFFFF if crc is None else crc)
    path.write_bytes(header + ttf)


class EmojiPackGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = Path(self.tmp.name) / "emoji.dat"

    def tearDown(self):
        self.tmp.cleanup()

    def test_matching_pack_is_accepted(self):
        make_emoji_pack(self.pack)
        self.assertIn("CRC ok", pkg.check_emoji_pack(self.pack))

    def test_bad_magic_is_refused(self):
        self.pack.write_bytes(b"NOPE" + b"\0" * 64)
        with self.assertRaises(SystemExit):
            pkg.check_emoji_pack(self.pack)

    def test_bad_version_is_refused(self):
        make_emoji_pack(self.pack, version=2)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_emoji_pack(self.pack)
        self.assertIn("version", str(ctx.exception))

    def test_crc_mismatch_is_refused(self):
        make_emoji_pack(self.pack, crc=0xDEADBEEF)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_emoji_pack(self.pack)
        self.assertIn("CRC", str(ctx.exception))

    def test_truncated_payload_is_refused(self):
        make_emoji_pack(self.pack, size=2048)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_emoji_pack(self.pack)
        self.assertIn("file size", str(ctx.exception))

    def test_oversized_ttf_is_refused(self):
        make_emoji_pack(self.pack, ttf=b"\0" * (pkg.EMOJI_SLOT), size=pkg.EMOJI_SLOT)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_emoji_pack(self.pack)
        self.assertIn("ttfSize", str(ctx.exception))


class AuxPackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = Path(self.tmp.name) / "pinyin.dat"

    def tearDown(self):
        self.tmp.cleanup()

    def test_pinyin_pack_is_accepted(self):
        self.pack.write_bytes(b"PYDB" + b"\0" * 4096)
        self.assertIn("pinyin", pkg.check_aux_pack(self.pack, b"PYDB", "pinyin", 0x80000))

    def test_hyphen_pack_is_accepted(self):
        self.pack.write_bytes(b"HYPN" + b"\0" * 4096)
        self.assertIn("hyphenation", pkg.check_aux_pack(self.pack, b"HYPN", "hyphenation", 0x70000))

    def test_bad_magic_is_refused(self):
        self.pack.write_bytes(b"NOPE" + b"\0" * 4096)
        with self.assertRaises(SystemExit):
            pkg.check_aux_pack(self.pack, b"PYDB", "pinyin", 0x80000)

    def test_pack_that_would_cross_the_next_slot_is_refused(self):
        self.pack.write_bytes(b"PYDB" + b"\0" * 0x80000)
        with self.assertRaises(SystemExit) as ctx:
            pkg.check_aux_pack(self.pack, b"PYDB", "pinyin", 0x80000)
        self.assertIn("next pack", str(ctx.exception))


class SbomTest(unittest.TestCase):
    def test_lock_versions_are_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp) / "dependencies.lock"
            lock.write_text(
                "dependencies:\n"
                "  espressif/esp_tinyusb:\n"
                "    component_hash: abc\n"
                "    version: 2.4.0\n"
                "  espressif/tinyusb:\n"
                "    version: 0.21.0~4\n",
                encoding="utf-8")
            self.assertEqual(
                pkg.read_lock_versions(lock),
                [("espressif/esp_tinyusb", "2.4.0"), ("espressif/tinyusb", "0.21.0~4")])

    def test_sbom_names_managed_components(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock = Path(tmp) / "dependencies.lock"
            lock.write_text("dependencies:\n  espressif/esp_tinyusb:\n    version: 2.4.0\n", encoding="utf-8")
            text = pkg.build_sbom(Path(tmp), lock, "v6.1", "esp32s31")
            self.assertIn("espressif/esp_tinyusb 2.4.0", text)
            self.assertIn("idf: v6.1", text)
            self.assertIn("UNTESTED ON HARDWARE", text)


class ImageGateTest(unittest.TestCase):
    def test_image_label_is_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "shell.bin"
            data = bytearray(0x80)
            struct.pack_into("<I", data, 0x20, 0xABCD5432)
            data[0x30:0x50] = b"0.0.0-untested".ljust(0x20, b"\0")
            data[0x50:0x70] = b"cpyos_metalio_plus".ljust(0x20, b"\0")
            image.write_bytes(data)
            project, version = pkg.describe(image)
            self.assertEqual(project, "cpyos_metalio_plus")
            self.assertEqual(version, "0.0.0-untested")

    def test_missing_app_desc_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "bad.bin"
            image.write_bytes(b"\0" * 0x80)
            with self.assertRaises(SystemExit):
                pkg.describe(image)


if __name__ == "__main__":
    unittest.main()
