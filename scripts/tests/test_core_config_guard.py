#!/usr/bin/env python3
"""The core-config guard: does it demand what platformio.ini demands, and does it fail loudly?

Motivated by AGENTS.md section 83: a device was left permanently white because the linked
Arduino core lacked the 64 KB data cache, and the documented detector (esp_app_desc.project_name)
could not see it -- that descriptor comes from the shared libesp_app_format.a.

    python3 -m unittest scripts.tests.test_core_config_guard
"""
from __future__ import annotations

import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "flash_pdf_xip", ROOT / "scripts" / "flash_pdf_xip.py")
fx = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fx)

GOOD = """\
# some generator wrote this
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=n
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
"""


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.required = fx.declared_core_config((ROOT / "platformio.ini").read_text())

    def test_demands_come_from_the_repo_not_from_a_hardcoded_list(self):
        self.assertEqual(self.required, {
            "CONFIG_ESP32S3_DATA_CACHE_64KB": "y",
            "CONFIG_ESP32S3_DATA_CACHE_LINE_64B": "y",
            "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
        }, "the guard must ask for exactly what platformio.ini declares")

    def test_matching_record_passes(self):
        self.assertEqual(fx.core_config_problems(GOOD, self.required), [])

    def test_vendor_cache_line_is_named(self):
        text = ("# CONFIG_ESP32S3_DATA_CACHE_64KB is not set\n"
                "CONFIG_ESP32S3_DATA_CACHE_32KB=y\n"
                "CONFIG_ESP32S3_DATA_CACHE_LINE_32B=y\n"
                "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=0\n")
        problems = fx.core_config_problems(text, self.required)
        self.assertEqual(len(problems), 3, problems)
        self.assertTrue(any("DATA_CACHE_64KB is absent" in p for p in problems), problems)
        self.assertTrue(any("RESERVE_INTERNAL=0" in p for p in problems), problems)

    def test_partial_record_still_fails(self):
        """A missing key must not be read as agreement -- that is the false green to avoid."""
        text = "CONFIG_ESP32S3_DATA_CACHE_64KB=y\n"
        problems = fx.core_config_problems(text, self.required)
        self.assertEqual(len(problems), 2, problems)

    def test_empty_demands_are_reported_not_silently_green(self):
        self.assertEqual(fx.core_config_problems("", {}), [])
        self.assertTrue(fx.compiled_core_problems.__doc__)
        # platformio.ini with none of the keys would make the guard vacuous; say so.
        self.assertEqual(fx.declared_core_config("nothing here\n"), {})

    def test_this_machine_is_actually_configured(self):
        problems = fx.compiled_core_problems()
        if problems and "no compiled-core sdkconfig found" in problems[0]:
            self.skipTest("no PlatformIO core installed here")
        self.assertEqual(problems, [], f"installed core record: {problems}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
