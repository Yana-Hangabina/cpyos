#!/usr/bin/env python3
"""Keep engineering doc §2.7 tied to real vendor code.

The sleep section claims a specific vendor sequence and names functions from the
vendored BSP and from the vendor application layer.  Those claims must not drift
away from the sources the next person will read.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DOC = ROOT / "docs" / "engineering" / "metalio-eink4-plus.md"
BSP_HEADER = (ROOT / "ports" / "metalio_plus" / "components" / "cpyos_metalio_bsp"
              / "vendor" / "board" / "display" / "epd_board_metalio_eink4_plus.h")
VENDOR_TREES = [
    ROOT.parents[1] / "repos" / "metalio-e-ink4-plus",
    ROOT.parents[1] / ".scratch" / "metalio-research" / "vendor",
    ROOT.parents[2] / ".scratch" / "metalio-research" / "vendor",
]


def vendor_power_file(name: str) -> Path | None:
    for tree in VENDOR_TREES:
        candidate = tree / "main" / "boards" / "common" / "power_policy" / name
        if candidate.is_file():
            return candidate
    return None


class VendorSleepDocsTest(unittest.TestCase):
    def setUp(self):
        self.doc = DOC.read_text(encoding="utf-8").split("### 2.7", 1)[1].split("## 3.", 1)[0]

    def test_section_names_the_vendored_primitives(self):
        header = BSP_HEADER.read_text(encoding="utf-8")
        for name in ("tps_enter_sleep", "bus_latch_for_sleep", "sleep_cut_rails",
                     "ioexp_mask_for_sleep", "ioexp_enable_power_wake_pin",
                     "ioexp_restore_keys"):
            self.assertIn(name, self.doc)
            self.assertIn(name, header)

    def test_section_states_the_wake_contract(self):
        self.assertIn("ESP_EXT1_WAKEUP_ANY_LOW", self.doc)
        self.assertIn("GPIO2", self.doc)
        self.assertIn("60 s", self.doc)
        self.assertIn("9726078b", self.doc)

    def test_reference_sequence_matches_the_documented_order(self):
        source = vendor_power_file("power_hw.cc")
        if source is None:
            self.skipTest("vendor power_policy tree not present")
        text = source.read_text(encoding="utf-8", errors="replace")
        body = text.split("esp_err_t power_hw_light_sleep_once(", 1)[1].split(
            "\nvoid power_hw_gpio_wake_latch_get", 1)[0]
        calls = ["power_hw_sd_park", "tps_enter_sleep", "bus_hold_low",
                 "MetalioAccel_ReleaseIntLine", "MetalioKeys_MaskForSleep",
                 "ioexp_enable_power_wake_pin", "EnableGpio2Wake",
                 "CutStandbyRails", "bus_latch_for_sleep", "s31_sleep::LightSleep"]
        positions = []
        for call in calls:
            index = body.find(call + "(")
            self.assertNotEqual(index, -1, f"reference no longer calls {call}")
            positions.append(index)
        self.assertEqual(positions, sorted(positions), "documented order drifted")

    def test_reference_wake_is_any_low_on_the_expander_int(self):
        source = vendor_power_file("power_hw.cc")
        if source is None:
            self.skipTest("vendor power_policy tree not present")
        text = source.read_text(encoding="utf-8", errors="replace")
        self.assertIn("ESP_EXT1_WAKEUP_ANY_LOW", text)
        self.assertIn("IO_EXPANDER_INT_GPIO", text)
        self.assertIn("esp_sleep_enable_timer_wakeup", text)


if __name__ == "__main__":
    unittest.main()
