#!/usr/bin/env python3
"""Static contract checks for the XIP PDF module and partition layout."""
from __future__ import annotations

import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build" / "pdfmod.bin"
PART = ROOT / "partitions_readpico_xip.csv"


class PdfModuleContractTest(unittest.TestCase):
    def test_xip_partitions_leave_data_at_the_same_base(self) -> None:
        rows = {}
        for line in PART.read_text(encoding="utf-8").splitlines():
            if not line or line.lstrip().startswith("#"):
                continue
            fields = [x.strip() for x in line.split(",")]
            if len(fields) >= 5:
                rows[fields[0]] = (int(fields[3], 0), int(fields[4], 0))
        self.assertEqual(rows["spiffs"][0], 0xCD0000)
        self.assertEqual(rows["app0"][1], 0x580000)
        self.assertEqual(rows["app1"][0], rows["app0"][0] + rows["app0"][1])
        self.assertEqual(rows["pdfmod"][0], rows["app1"][0] + rows["app1"][1])
        self.assertEqual(rows["spiffs"][0], rows["pdfmod"][0] + rows["pdfmod"][1])
        for name, (offset, size) in rows.items():
            self.assertLessEqual(offset + size, 16 * 1024 * 1024, name)

    def test_module_header_and_checksums(self) -> None:
        if not IMAGE.is_file():
            self.skipTest("build/pdfmod.bin not built")
        data = IMAGE.read_bytes()
        self.assertGreater(len(data), 0x10000)
        magic, version, abi, host_api, layout_key = struct.unpack("<IHHII", data[:16])
        (header_bytes, code_offset, entry_offset, code_size, code_crc, rodata_offset, rodata_size,
         rodata_crc, data_offset, data_size, data_crc, bss_size, engine_rev) = struct.unpack("<13I", data[16:68])
        build_id = data[68:100]
        self.assertEqual(magic, 0x31504D43)
        self.assertEqual((version, abi), (2, 4))
        self.assertEqual(host_api, 0x00010000)
        import re
        keys = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;",
                          (ROOT / "src/pdf/PdfModuleApi.h").read_text())
        self.assertEqual(layout_key, int(keys[1], 16))  # readpico flash mapping, from the header
        self.assertGreater(engine_rev, 0)
        self.assertEqual((header_bytes, code_offset), (0x10000, 0x10000))
        self.assertLess(entry_offset, code_size)
        self.assertLessEqual(data_offset + data_size, len(data))
        self.assertEqual(zlib.crc32(data[code_offset:code_offset + code_size]) & 0xFFFFFFFF, code_crc)
        self.assertEqual(zlib.crc32(data[rodata_offset:rodata_offset + rodata_size]) & 0xFFFFFFFF, rodata_crc)
        self.assertEqual(zlib.crc32(data[data_offset:data_offset + data_size]) & 0xFFFFFFFF, data_crc)
        self.assertLessEqual(rodata_offset + rodata_size, data_offset)
        self.assertLessEqual(data_offset + data_size, 0x1E0000)
        self.assertGreater(bss_size, 0)
        self.assertGreaterEqual(rodata_size, 0)
        self.assertTrue(any(build_id), "buildId must not be all-zero")
        # buildId is provenance: it must be the ELF the module was linked from.
        elf = ROOT / ".pio" / "build" / "readpico_pdf" / "firmware.elf"
        if elf.is_file():
            import hashlib
            self.assertEqual(build_id, hashlib.sha256(elf.read_bytes()).digest())


if __name__ == "__main__":
    unittest.main()
