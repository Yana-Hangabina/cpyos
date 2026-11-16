#!/usr/bin/env python3
"""The generated FilesPage literal must inline its includes and stay stable."""

import importlib.util
import re
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "gen_web_pages.py"
spec = importlib.util.spec_from_file_location("gen_web_pages", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class WebPagesTest(unittest.TestCase):
    def test_pages_are_inlined_into_literals(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "web_pages.inc"
            sys.argv = ["gen_web_pages.py", "--out", str(out)]
            self.assertEqual(mod.main(), 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn("kFilesPageEn[]", text)
            self.assertIn("kFilesPageZh[]", text)
            self.assertIn("kFontsPageEn[]", text)
            self.assertIn("kFontsPageZh[]", text)
            self.assertIn("kHomePageEn[]", text)
            self.assertIn("kHomePageZh[]", text)
            self.assertNotIn("<!--#include", text)
            self.assertGreater(len(text), 400_000)

            def page(name: str) -> str:
                body = text.split(f"{name}[] = {{", 1)[1].split("};", 1)[0]
                return zlib.decompress(bytes(int(v) for v in re.findall(r"\d+", body)), 16 + zlib.MAX_WBITS).decode("utf-8")

            for name in ("kFilesPageEn", "kFilesPageZh", "kFontsPageEn", "kFontsPageZh", "kHomePageEn"):
                self.assertTrue(page(name), name)
            every_page = "\n".join(
                page(n)
                for n in ("kFilesPageEn", "kFilesPageZh", "kFontsPageEn", "kFontsPageZh", "kHomePageEn", "kHomePageZh")
            )
            self.assertIn("PLUS: { width: 1216, height: 684", every_page)
            self.assertIn("cpyos_metalio_plus: 'PLUS'", every_page)

    def test_generation_is_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "a.inc"
            second = Path(tmp) / "b.inc"
            for path in (first, second):
                sys.argv = ["gen_web_pages.py", "--out", str(path)]
                mod.main()
            self.assertEqual(first.read_bytes(), second.read_bytes())


if __name__ == "__main__":
    unittest.main()
