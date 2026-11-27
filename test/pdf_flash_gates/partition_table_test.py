#!/usr/bin/env python3
"""Host tests for the partition-table reader and the layout-shift gate.

Why: the five data blobs are read at absolute offsets inside `spiffs`.  This tree's
two readpico tables deliberately put spiffs at the same place, so switching between
them is safe -- but the *device* may be sitting on another branch's layout, and
writing a table that moves spiffs while flashing only the app reproduces AGENTS.md
section 29.1 exactly: the device boots, pinyin/hyphenation/language pack are
silently dead, and esptool printed `Hash of data verified.` for everything it wrote.
The gate is the only place that catches it, so the parsing it rests on is tested
here against real bytes, not just hand-made ones.
"""
from __future__ import annotations

import csv
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "scripts"))

from flash_pdf_xip import layout_shift_message, parse_partition_table, partitions  # noqa: E402


def entry(label: str, offset: int, size: int, sub_type: int = 0x40) -> bytes:
    name = label.encode("latin1")[:15].ljust(16, b"\0")
    return (b"\xaa\x50" + bytes([0x01, sub_type])
            + offset.to_bytes(4, "little") + size.to_bytes(4, "little") + name + b"\0" * 4)


def table_of(rows: list[tuple[str, int, int]]) -> bytes:
    return b"".join(entry(label, off, size) for label, off, size in rows) + b"\xff" * 32


class PartitionTableTest(unittest.TestCase):
    def test_parses_entries_and_stops_at_blank_space(self) -> None:
        blob = table_of([("nvs", 0x9000, 0x5000), ("spiffs", 0xCD0000, 0x320000)])
        rows = parse_partition_table(blob)
        self.assertEqual(rows["spiffs"], (0xCD0000, 0x320000))
        self.assertEqual(rows["nvs"], (0x9000, 0x5000))
        self.assertNotIn("\xff", rows)

    def test_empty_blob_is_an_empty_table(self) -> None:
        self.assertEqual(parse_partition_table(b"\xff" * 64), {})

    def test_same_spiffs_is_not_a_shift(self) -> None:
        device = {"spiffs": (0xCD0000, 0x320000)}
        target = {"spiffs": (0xCD0000, 0x320000)}
        self.assertIsNone(layout_shift_message(device, target))

    def test_moved_spiffs_is_refused(self) -> None:
        # The real risk: a device last flashed from a branch whose data partition
        # lives at 0xE70000.
        device = {"spiffs": (0xE70000, 0x180000)}
        target = partitions()
        message = layout_shift_message(device, target)
        self.assertIsNotNone(message)
        self.assertIn("0xe70000", message or "")
        self.assertIn("--full", message or "")

    def test_resized_spiffs_is_also_a_shift(self) -> None:
        target = partitions()
        base, _size = target["spiffs"]
        self.assertIsNotNone(layout_shift_message({"spiffs": (base, 0x360000)}, target))

    def test_unknown_sides_cannot_claim_safety(self) -> None:
        self.assertIsNone(layout_shift_message({}, partitions()))
        self.assertIsNone(layout_shift_message(partitions(), {}))

    def test_this_trees_two_tables_share_spiffs(self) -> None:
        """The doc's 'same base and size' claim, checked against the csv files."""
        normal = REPO / "partitions_readpico.csv"
        if not normal.is_file():
            self.skipTest(f"{normal} not present in this checkout")
        rows: dict[str, tuple[int, int]] = {}
        with normal.open(newline="", encoding="utf-8") as stream:
            for row in csv.reader(line for line in stream if not line.lstrip().startswith("#")):
                if len(row) >= 5:
                    rows[row[0].strip()] = (int(row[3].strip(), 0), int(row[4].strip(), 0))
        self.assertIsNone(layout_shift_message({"spiffs": rows["spiffs"]}, partitions()))

    def test_parser_agrees_with_the_built_partition_image(self) -> None:
        """Against the real partitions.bin, so a wrong struct layout cannot pass."""
        built = REPO / ".pio" / "build" / "readpico_pdf" / "partitions.bin"
        if not built.is_file():
            self.skipTest("build -e readpico_pdf first")
        rows = parse_partition_table(built.read_bytes())
        expected = partitions()
        for label, where in expected.items():
            self.assertIn(label, rows, f"{label} missing from the built table")
            self.assertEqual(rows[label], where, f"{label} decoded wrong")


if __name__ == "__main__":
    unittest.main(verbosity=2)
