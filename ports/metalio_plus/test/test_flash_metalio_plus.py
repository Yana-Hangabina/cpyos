#!/usr/bin/env python3
"""Host tests for the Plus flash planner (no serial port is touched)."""

import argparse
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "flash_metalio_plus.py"
spec = importlib.util.spec_from_file_location("flash_plus", SCRIPT)
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def make_table(path: Path, csv=flash.PORT_PARTITIONS_CSV):
    rows = flash.parse_partition_rows(csv)
    blob = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, 0, 0, row["offset"], row["size"],
                    row["name"].encode(), 0)
        for row in rows)
    path.write_bytes(blob)


def make_image(path: Path, project="cpyos_metalio_plus", version="0.0.0-untested", size=0x100):
    data = bytearray(size)
    struct.pack_into("<I", data, flash.APP_DESC_OFFSET, flash.APP_DESC_MAGIC)
    data[0x30:0x50] = version.encode()[:0x20].ljust(0x20, b"\0")
    data[0x50:0x70] = project.encode()[:0x20].ljust(0x20, b"\0")
    path.write_bytes(data)


class FlashPlanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fw = self.dir / "app.bin"
        self.pinyin = self.dir / "pinyin.dat"
        self.hyphen = self.dir / "hyphen.dat"
        self.i18n = self.dir / "i18n.dat"
        self.emoji = self.dir / "emoji.dat"
        self.web = self.dir / "web.dat"
        self.core = self.dir / "core.dat"
        self.table = self.dir / "partition-table.bin"
        make_image(self.fw)
        self.pinyin.write_bytes(b"PYDB" + b"\0" * 96)
        self.hyphen.write_bytes(b"HYPN" + b"\0" * 96)
        self.i18n.write_bytes(b"JILP" + b"\0" * 96)
        self.emoji.write_bytes(b"EMJ1" + b"\0" * 96)
        self.web.write_bytes(b"CWP1" + b"\0" * 96)
        self.core.write_bytes(b"CPFZ" + b"\0" * 96)
        make_table(self.table)

    def tearDown(self):
        self.tmp.cleanup()

    def args(self, **kwargs):
        values = dict(firmware=self.fw, pinyin=self.pinyin, hyphen=self.hyphen,
                      i18n=self.i18n, emoji=self.emoji, web=self.web, core=self.core, no_data=False,
                      layout="port", full_install=True, slot="both",
                      partition_table=self.table)
        values.update(kwargs)
        return argparse.Namespace(**values)

    def test_segments_use_the_port_offsets(self):
        segments, version = flash.build_plan(self.args())
        self.assertEqual(version, "0.0.0-untested")
        self.assertEqual([offset for offset, _ in segments],
                         [0x8000, 0x10000, 0x9B0000, 0xA40000, 0xAB0000, 0xB10000, 0xB30000])

    def test_no_data_keeps_the_table_and_app(self):
        segments, _ = flash.build_plan(self.args(no_data=True))
        self.assertEqual([offset for offset, _ in segments], [0x8000, 0x10000])

    def test_vendor_layout_uses_the_stock_slots(self):
        segments, version = flash.build_plan(
            self.args(layout="vendor", full_install=False, no_data=True))
        self.assertEqual(version, "0.0.0-untested")
        self.assertEqual([offset for offset, _ in segments], [0x80000, 0x580000])

    def test_vendor_layout_can_use_one_slot(self):
        segments, _ = flash.build_plan(
            self.args(layout="vendor", full_install=False, no_data=True, slot="ota_1"))
        self.assertEqual([offset for offset, _ in segments], [0x580000])

    def test_vendor_layout_refuses_the_full_shell(self):
        make_image(self.fw, size=0x500001)
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args(layout="vendor", full_install=False, no_data=True))
        self.assertIn("5 MiB", str(ctx.exception))

    def test_vendor_layout_refuses_data_packs(self):
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args(layout="vendor", full_install=False, no_data=False))
        self.assertIn("data packs", str(ctx.exception))

    def test_port_ota_plan_writes_both_slots_and_all_packs(self):
        make_table(self.table, flash.PORT_OTA_PARTITIONS_CSV)
        segments, version = flash.build_plan(self.args(layout="port-ota"))
        self.assertEqual(version, "0.0.0-untested")
        self.assertEqual([offset for offset, _ in segments],
                         [0x8000, 0x10000, 0x670000, 0xCD0000, 0xD26000, 0xD60000,
                          0xDD0000, 0xE30000, 0xE50000])

    def test_port_ota_app_only_writes_both_slots(self):
        segments, _ = flash.build_plan(self.args(layout="port-ota", full_install=False))
        self.assertEqual([offset for offset, _ in segments], [0x10000, 0x670000])

    def test_port_ota_refuses_an_oversized_image(self):
        make_image(self.fw, size=0x730001)
        make_table(self.table, flash.PORT_OTA_PARTITIONS_CSV)
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args(layout="port-ota"))
        self.assertIn("OTA slot", str(ctx.exception))

    def test_port_layout_refuses_a_stale_partition_table(self):
        make_table(self.table, flash.VENDOR_PARTITIONS_CSV)
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args())
        self.assertIn("does not match partitions.csv", str(ctx.exception))

    def test_port_layout_requires_an_explicit_full_install(self):
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args(full_install=False))
        self.assertIn("--full-install", str(ctx.exception))

    def test_read_pico_image_is_refused(self):
        make_image(self.fw, project="crossmux")
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args())
        self.assertIn("cpyos_metalio_plus", str(ctx.exception))

    def test_missing_untested_label_is_refused(self):
        make_image(self.fw, version="1.2.3")
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args())
        self.assertIn("untested", str(ctx.exception))

    def test_oversized_app_is_refused(self):
        parts = flash.parse_partitions(flash.PORT_DIR / "partitions.csv")
        slot = parts["ota_0"][1]
        make_image(self.fw, size=slot + 1)
        with self.assertRaises(SystemExit) as ctx:
            flash.build_plan(self.args())
        self.assertIn("exceeds", str(ctx.exception))

    def test_pack_overlap_is_refused(self):
        spiffs, size = 0xE70000, 0x180000
        with self.assertRaises(SystemExit) as ctx:
            flash.check_ranges([("i18n", spiffs + 0x100000, 0x70000),
                                ("emoji", spiffs + 0x160000, 0x100)], spiffs, size)
        self.assertIn("overlap", str(ctx.exception))

    def test_reserved_gap_is_protected(self):
        spiffs, size = 0xE70000, 0x180000
        with self.assertRaises(SystemExit) as ctx:
            flash.check_ranges([("bad", spiffs + flash.SPIFFS_RESERVED - 8, 32)], spiffs, size)
        self.assertIn("reserved", str(ctx.exception))

    def test_pack_outside_spiffs_is_refused(self):
        spiffs, size = 0xE70000, 0x180000
        with self.assertRaises(SystemExit):
            flash.check_ranges([("tail", spiffs + size - 8, 16)], spiffs, size)

    def test_reuse_backup_accepts_a_full_image(self):
        img = self.dir / "factory-16m.bin"
        img.write_bytes(b"\0" * flash.IMAGE_SIZE)
        self.assertEqual(len(flash.check_reuse_backup(img)), 64)

    def test_reuse_backup_rejects_a_short_image(self):
        img = self.dir / "short.bin"
        img.write_bytes(b"\0" * 1024)
        with self.assertRaises(SystemExit):
            flash.check_reuse_backup(img)

    def test_verify_success_accepts_both_esptool_wordings(self):
        self.assertTrue(flash.verify_success("Hash of data verified."))
        self.assertTrue(flash.verify_success("Verification successful (digest matched)."))
        self.assertFalse(flash.verify_success("Verification failed"))

    def test_stock_partition_sizes_are_parsed(self):
        parts = flash.parse_partitions(flash.VENDOR_PARTITIONS_CSV)
        self.assertEqual(parts["ota_0"], (0x80000, 5 * 1024 * 1024))
        self.assertEqual(parts["ota_1"], (0x580000, 5 * 1024 * 1024))
        self.assertEqual(parts["model"], (0x10000, 400 * 1024))

    def test_partition_table_binary_roundtrip(self):
        rows = flash.parse_partition_rows(flash.VENDOR_PARTITIONS_CSV)
        blob = b"".join(
            struct.pack("<HBBII16sI", 0x50AA, 0, 0, row["offset"], row["size"],
                        row["name"].encode(), 0)
            for row in rows)
        actual = flash.read_partition_table_binary(blob)
        self.assertEqual([r["name"] for r in actual], [r["name"] for r in rows])
        self.assertTrue(flash.tables_match(rows, actual))
        actual[0]["offset"] += 1
        self.assertFalse(flash.tables_match(rows, actual))


if __name__ == "__main__":
    unittest.main()
