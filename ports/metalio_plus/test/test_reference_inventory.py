#!/usr/bin/env python3
"""Mutation tests for the kiikoread disposition inventory."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "inventory_reference_app.py"
spec = importlib.util.spec_from_file_location("inventory_reference_app", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

STATUSES = {"ported", "reference", "replaced", "not-ported", "not-applicable", "build"}


class ReferenceInventoryTest(unittest.TestCase):
    def test_every_disposition_uses_a_known_status(self):
        for entry, (status, note) in mod.DISPOSITIONS.items():
            self.assertIn(status, STATUSES, entry)
            self.assertTrue(note.strip(), entry)

    def test_unclassified_entry_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            for area in ("main", "components", "boards"):
                (Path(tmp) / area).mkdir()
            (Path(tmp) / "main" / "brand_new_app.c").write_text("int x;", encoding="utf-8")
            rows, unknown = mod.collect(Path(tmp))
            self.assertEqual(unknown, ["main/brand_new_app.c"])
            self.assertEqual(rows, [])

    def test_reference_tree_is_fully_classified(self):
        tree = mod.default_tree()
        if tree is None:
            self.skipTest("reference tree not present")
        rows, unknown = mod.collect(tree)
        self.assertEqual(unknown, [])
        self.assertGreaterEqual(len(rows), 60)
        ported = {row["entry"] for row in rows if row["status"] == "ported"}
        self.assertEqual(ported, {"components/e0470_epaper_waveform/", "components/e0470_page_turn/"})


if __name__ == "__main__":
    unittest.main()
