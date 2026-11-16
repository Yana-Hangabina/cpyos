#!/usr/bin/env python3
"""Font-page wiring: routes, validation helpers and registry invalidation."""

import unittest
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
SOURCE = (PORT / "main" / "web_transfer.cpp").read_text(encoding="utf-8")
GEN = (PORT / "scripts" / "gen_web_pages.py").read_text(encoding="utf-8")


class WebFontsTest(unittest.TestCase):
    def test_routes_are_registered(self):
        for uri in ('"/api/fonts"', '"/api/fonts/upload"', '"/api/fonts/delete"', '"/fonts/en"'):
            self.assertIn(uri, SOURCE)

    def test_upload_and_delete_invalidate_the_registry(self):
        self.assertGreaterEqual(SOURCE.count("sdFontSystem.markRegistryDirty()"), 2)

    def test_official_pages_are_embedded(self):
        for symbol in ("kFilesPageZh", "kFontsPageZh", "kHomePageZh", "kHomePageEn"):
            self.assertIn(symbol, GEN)


if __name__ == "__main__":
    unittest.main()
