#!/usr/bin/env python3
"""Host test for the flash script's layout decision.

The bug being pinned is a *guard pointed at the wrong path*. The old code compared
the device's partition table with the image's only when `--full`/`--first-install`
was given, and refused if they differed -- i.e. it blocked the one invocation that
also rewrites the table and every data blob, and said nothing at all about an
app-only flash onto a device with a different table. That app-only flash is what
people actually run, and it is exactly AGENTS.md section 29.1's silent failure: the
device boots, the language pack / pinyin / hyphenation are dead, and esptool reports
`Hash of data verified.` for every byte it did write.
"""
import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("flash_pdf_xip", ROOT / "scripts/flash_pdf_xip.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

XIP = {"spiffs": (0x00CD0000, 0x00290000), "pdfmod": (0x00AF0000, 0x001E0000)}
DEV = {"spiffs": (0x00E70000, 0x00180000), "pdfmod": (0x00AF0000, 0x001E0000)}


class LayoutDecision(unittest.TestCase):
    def test_matching_tables_proceed(self):
        allowed, message = module.layout_decision(dict(XIP), XIP, full=False)
        self.assertTrue(allowed)
        self.assertIn("data blobs stay valid", message)

    def test_app_only_onto_a_different_table_refuses(self):
        """The case the old guard never looked at."""
        allowed, message = module.layout_decision(dict(DEV), XIP, full=False)
        self.assertFalse(allowed)
        self.assertIn("refusing", message)
        # and it names the remedy the caller can actually run
        self.assertIn("--full", message)

    def test_full_proceeds_because_it_rewrites_the_blobs_too(self):
        allowed, message = module.layout_decision(dict(DEV), XIP, full=True)
        self.assertTrue(allowed)
        self.assertIn("rewritten with it", message)

    def test_unreadable_table_is_not_a_pass(self):
        allowed, message = module.layout_decision(None, XIP, full=True)
        self.assertFalse(allowed)
        self.assertIn("cannot prove", message)

    def test_shift_message_still_describes_the_real_damage(self):
        why = module.layout_shift_message(DEV, XIP)
        self.assertIsNotNone(why)
        self.assertIn("0xe70000", why)
        self.assertIn("0xcd0000", why)
        self.assertIn("offsets", why)

    def test_identical_layouts_produce_no_complaint(self):
        self.assertIsNone(module.layout_shift_message(dict(XIP), XIP))
        # an unknown spiffs on either side must not invent a difference
        self.assertIsNone(module.layout_shift_message({}, XIP))
        self.assertIsNone(module.layout_shift_message(dict(XIP), {}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
