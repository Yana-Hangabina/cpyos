#!/usr/bin/env python3
"""Pinch must be sampled and must not leave a page-turn tap behind."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GPIO = ROOT / "ports" / "metalio_plus" / "components" / "cpyos_ui" / "HalGPIO.cpp"
DRIVER = (ROOT / "ports" / "metalio_plus" / "components" / "cpyos_metalio_bsp"
          / "vendor" / "board" / "display" / "metalio_touch.c")


class TouchInputTest(unittest.TestCase):
    def test_second_point_is_read(self):
        driver = DRIVER.read_text(encoding="utf-8")
        self.assertIn("uint8_t buf[10]", driver)
        self.assertIn("metalio_touch_read_pair", driver)

    def test_pinch_is_wired_and_consumed_once(self):
        gpio = GPIO.read_text(encoding="utf-8")
        self.assertIn("metalio_touch_read_pair(&pair, &pairSeq_)", gpio)
        self.assertIn("pinchPending_ = false;", gpio)

    def test_pinch_suppresses_the_release_tap(self):
        gpio = GPIO.read_text(encoding="utf-8")
        self.assertIn("const bool pinched = pinchGesture_;", gpio)
        self.assertIn("!pinched &&", gpio)


if __name__ == "__main__":
    unittest.main()
