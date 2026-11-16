#!/usr/bin/env python3
"""Refresh modes must reach the panel, not be collapsed into one waveform."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "ports" / "metalio_plus" / "components" / "cpyos_ui" / "HalDisplay.cpp"


class DisplayModesTest(unittest.TestCase):
    def setUp(self):
        self.text = SOURCE.read_text(encoding="utf-8")

    def test_push_takes_the_requested_mode(self):
        self.assertIn("void pushFrameBuffer(HalDisplay::RefreshMode mode, DisplayRefreshContext context)", self.text)
        self.assertIn(
            "void HalDisplay::refreshDisplay(RefreshMode mode, bool) { pushFrameBuffer(mode, "
            "DisplayRefreshContext::Normal); }",
            self.text,
        )

    def test_half_and_fast_use_a_differential_waveform(self):
        # FULL is the only non-differential GC16 sweep; transient frames ride
        # the DU table and everything else the differential GL16 one.
        self.assertIn("MODE_GC16", self.text)
        self.assertIn("MODE_DU", self.text)
        self.assertIn("MODE_GL16", self.text)
        self.assertNotIn("MODE_GC16 : MODE_GL16", self.text)

    def test_first_push_and_grayscale_stay_full(self):
        self.assertIn("epd_hl_update_screen_from_white(hl, MODE_GC16, 25)", self.text)
        gray = self.text.split("void pushGrayBuffer(bool clean)", 1)[1].split("void blitBwRect", 1)[0]
        self.assertIn("MODE_GC16", gray)


if __name__ == "__main__":
    unittest.main()
