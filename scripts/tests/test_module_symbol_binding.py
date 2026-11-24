#!/usr/bin/env python3
"""The engine module must not be linked against the app at all.

History: the builder once exported every app global symbol as an absolute
address (20,790 defsyms).  That both invalidated the engine on every rebuild and
handed it linker layout symbols (`_ext_ram_bss_start`), which zeroed live app
state and crashed lwIP.  The host-service table replaced that model; this test
pins the replacement so the old model cannot come back silently.

    python3 -m unittest scripts.tests.test_module_symbol_binding
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILDER = ROOT / "scripts" / "build_pdf_module.py"


class StaticBuilderTest(unittest.TestCase):
    def test_builder_exports_no_app_symbols(self):
        text = BUILDER.read_text(encoding="utf-8")
        for forbidden in ("host_provides", "--just-symbols", "--defsym"):
            self.assertNotIn(forbidden, text,
                             f"{forbidden} would bind the module to one exact app layout")
        self.assertIn("PdfModuleHostGlue.h", text + (ROOT / "src" / "pdf" / "PdfDocument.cpp").read_text(),
                      "the module must include the host shim instead of taking app symbols")
        self.assertIn("PdfModuleGlue.cpp", text,
                      "the module runtime glue (malloc/new/hooks) must be linked in")

    def test_document_source_uses_the_host_table_in_module_builds(self):
        text = (ROOT / "src" / "pdf" / "PdfDocument.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "PdfModuleHostGlue.h"', text,
                      "the module build must include the host shim, not HalStorage/heap headers")
        self.assertNotIn("logSerial.printf", text,
                         "the module has no Serial object; logs must use LOG_* -> host->log")


def _nm() -> str:
    found = shutil.which("riscv32-esp-elf-nm") or shutil.which("xtensa-esp32s3-elf-nm")
    if found:
        return found
    for candidate in (
            pathlib.Path.home() / ".espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-nm",
            pathlib.Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"):
        if candidate.exists():
            return str(candidate)
    return ""


class ModuleElfTest(unittest.TestCase):
    def setUp(self):
        self.elf = ROOT / "build" / "pdfmod.elf"
        self.nm = _nm()
        if not self.elf.exists() or not self.nm:
            self.skipTest("module ELF or nm not available")
        self.symbols = subprocess.run([self.nm, str(self.elf)], capture_output=True, text=True).stdout

    def test_no_absolute_host_symbols(self):
        absolute = [ln for ln in self.symbols.splitlines() if len(ln.split()) == 3 and ln.split()[1] == "A"]
        self.assertEqual([], absolute, f"module still binds app addresses: {absolute[:3]}")

    def test_no_undefined_symbols(self):
        undefined = [ln for ln in self.symbols.splitlines() if len(ln.split()) == 2 and ln.split()[0] == "U"]
        self.assertEqual([], undefined, f"module has external references: {undefined[:3]}")

    def test_no_app_layout_symbols(self):
        self.assertNotIn("_ext_ram_bss", self.symbols)


if __name__ == "__main__":
    unittest.main(verbosity=2)
