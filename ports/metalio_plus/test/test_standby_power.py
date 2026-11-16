#!/usr/bin/env python3
"""Standby must not leave the frontlight (or a radio) draining the battery."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SHELL = ROOT / "ports" / "metalio_plus" / "main" / "shell_demo.cpp"


class StandbyPowerTest(unittest.TestCase):
    def setUp(self):
        self.text = SHELL.read_text(encoding="utf-8")

    def test_frontlight_is_turned_off_entering_standby(self):
        block = self.text.split("standbyLightOff", 1)[1].split("// The per-frame press latches", 1)[0]
        self.assertIn("Frontlight.isOn()", block)
        self.assertIn("Frontlight.setOn(false)", block)

    def test_restore_only_when_the_user_wants_light(self):
        block = self.text.split("standbyLightOff", 1)[1].split("// The per-frame press latches", 1)[0]
        self.assertIn("if (SETTINGS.frontlightOn) Frontlight.setOn(true);", block)

    def test_boot_logs_reset_reason_and_wake_cause(self):
        self.assertIn("plus boot: reset=%d wake=%d", self.text)
        self.assertIn("esp_reset_reason()", self.text)

    def test_auto_standby_still_requires_the_radio_off(self):
        self.assertIn("WiFi.getMode()) == static_cast<int>(WIFI_MODE_NULL)", self.text)


if __name__ == "__main__":
    unittest.main()
