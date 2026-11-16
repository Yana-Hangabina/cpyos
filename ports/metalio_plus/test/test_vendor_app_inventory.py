#!/usr/bin/env python3
"""Mutation tests for the vendor application inventory."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "inventory_vendor_app.py"
spec = importlib.util.spec_from_file_location("inventory_vendor_app", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

STATUSES = {"ported", "reference", "replaced", "not-ported", "not-applicable", "build"}


class VendorInventoryTest(unittest.TestCase):
    def test_every_disposition_uses_a_known_status(self):
        for entry, (status, note) in mod.DISPOSITIONS.items():
            self.assertIn(status, STATUSES, entry)
            self.assertTrue(note.strip(), entry)

    def test_unclassified_entry_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            main = Path(tmp) / "main"
            main.mkdir()
            (main / "brand_new_service").mkdir()
            (main / "app_main.cc").write_text("int main(){}", encoding="utf-8")
            rows, unknown = mod.collect(Path(tmp))
            self.assertEqual(unknown, ["brand_new_service"])
            self.assertEqual([row["entry"] for row in rows], ["app_main.cc"])

    def test_reference_tree_is_fully_classified(self):
        tree = mod.default_tree()
        if tree is None:
            self.skipTest("vendor tree not present")
        rows, unknown = mod.collect(tree)
        self.assertEqual(unknown, [])
        self.assertGreaterEqual(len(rows), 40)
        # Pin the entries that are actually implemented rather than a count, so
        # a new "ported" claim has to be a deliberate edit here (audio was added
        # 2026-11-15; before that this set held only the BSP and haptics).
        ported = {row["entry"] for row in rows if row["status"] == "ported"}
        self.assertEqual(ported, {"boards/", "audio/", "battery_uv_lockout.h",
                                  "haptic_feedback.cc", "haptic_feedback.h"})


if __name__ == "__main__":
    unittest.main()
