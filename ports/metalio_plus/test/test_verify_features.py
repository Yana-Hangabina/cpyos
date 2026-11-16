#!/usr/bin/env python3
"""Mutation tests for the feature-presence checker.

The checker only earns trust if it fails on a stripped image.  These tests feed
it synthetic images: one complete, one missing a marker, and one where a
shell-only marker leaks into the reader variant.
"""

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "verify_port_features.py"
spec = importlib.util.spec_from_file_location("verify_features", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class FeatureCheckerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, markers):
        path = self.dir / name
        path.write_bytes(b"\0" * 64 + b"\n".join(markers))
        return path

    def test_complete_shell_image_passes(self):
        image = self.write("shell.bin", list(mod.SHELL_MARKERS.values()))
        self.assertEqual(mod.check_markers(image, mod.SHELL_MARKERS), [])

    def test_stripped_marker_is_reported(self):
        markers = list(mod.SHELL_MARKERS.values())
        markers.remove(b"PROPFIND")
        image = self.write("shell.bin", markers)
        self.assertEqual(mod.check_markers(image, mod.SHELL_MARKERS), ["webdav methods"])

    def test_shell_marker_in_reader_is_reported(self):
        image = self.write("reader.bin", [b"encryption.xml", b"epub", b"/api/settings"])
        self.assertIn("/api/settings", mod.check_absent(image, mod.SHELL_ONLY_MARKERS))

    def test_reader_markers_pass(self):
        image = self.write("reader.bin", list(mod.READER_MARKERS.values()))
        self.assertEqual(mod.check_markers(image, mod.READER_MARKERS), [])

    def test_private_marker_in_public_image_is_reported(self):
        image = self.write("shell.bin", [b"hello", b"VIP chain"])
        self.assertEqual(mod.check_absent(image, mod.FORBIDDEN_MARKERS), ["VIP chain"])

    def test_missing_symbol_is_reported(self):
        symbols = "T web_transfer5startEv\nT settingsHandler\n"
        self.assertEqual(mod.check_symbols(symbols, ("settingsHandler", "usb_msc")), ["usb_msc"])

    def test_closed_source_symbol_leak_is_reported(self):
        symbols = "T _ZN12JjwxcActivity5onEnterEv\n"
        self.assertEqual(mod.forbidden_symbols(symbols, mod.FORBIDDEN_SYMBOLS), ["JjwxcActivity"])
        self.assertEqual(mod.forbidden_symbols("T web_transfer5startEv\n", mod.FORBIDDEN_SYMBOLS), [])

    def test_bsp_markers_pass(self):
        image = self.write("bsp.bin", list(mod.BSP_MARKERS.values()))
        self.assertEqual(mod.check_markers(image, mod.BSP_MARKERS), [])


if __name__ == "__main__":
    unittest.main()
