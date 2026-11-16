"""Unit tests for scripts/analyze_power_log.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

import analyze_power_log  # noqa: E402

LOG = """\
boot reset=3 cause=0 ms=685
sample ms=60000 mv=4355 ma=-75 soc=100 chg=0 usb=0 wifi=0
sample ms=120000 mv=4352 ma=-76 soc=100 chg=0 usb=0 wifi=0
sample ms=180000 mv=4350 ma=-74 soc=99 chg=0 usb=0 wifi=0
previous-wake reason=1 stage=14
boot reset=4 cause=0 stage=14 ms=773
wake reason=1 stage=14 ms=76066 mv=4372 ma=5 soc=100 chg=0 usb=1
wake reason=0 stage=14 ms=90000 mv=4370 ma=-70 soc=100 chg=0 usb=0
usb=0 ms=94455
usb=1 ms=1512
"""


class AnalyzePowerLogTest(unittest.TestCase):
    def test_parses_boots_wakes_and_usb(self) -> None:
        summary = analyze_power_log.analyze(LOG)
        self.assertEqual(len(summary.boots), 2)
        self.assertEqual(summary.boots[0]["reset"], 3)
        self.assertEqual(summary.boots[1]["stage"], 14)
        self.assertEqual(len(summary.power_wakes()), 1)
        self.assertEqual(len(summary.timer_wakes()), 1)
        self.assertEqual(summary.failed_wakes(), [])
        self.assertEqual(summary.previous_wakes[0]["reason"], 1)
        self.assertEqual([u["usb"] for u in summary.usb], [0, 1])
        self.assertEqual(summary.panics, [])

    def test_battery_curve(self) -> None:
        summary = analyze_power_log.analyze(LOG)
        curve = analyze_power_log.battery_curve(summary.samples)
        self.assertEqual(curve["count"], 3)
        self.assertAlmostEqual(curve["minutes"], 2.0)
        self.assertAlmostEqual(curve["mean_ma"], -75.25)
        self.assertAlmostEqual(curve["charge_mah"], -2.5083, places=3)
        self.assertEqual(curve["mv_delta"], -5)
        self.assertEqual(curve["soc_delta"], -1)
        self.assertTrue(curve["all_usb0"])

    def test_curve_needs_two_samples(self) -> None:
        summary = analyze_power_log.analyze("sample ms=1000 mv=4300 ma=-70 soc=90 chg=0 usb=0")
        self.assertEqual(analyze_power_log.battery_curve(summary.samples), {})

    def test_report_names_reasons(self) -> None:
        text = analyze_power_log.report(LOG)
        self.assertIn("boot reset=sw", text)
        self.assertIn("boot reset=panic stage=14", text)
        self.assertIn("wake reason=power stage=14", text)
        self.assertIn("wake reason=timer stage=14", text)
        self.assertIn("mean current -75.2 mA", text)
        self.assertIn("all usb=0: yes", text)

    def test_last_session_is_the_cable_out_window(self) -> None:
        text = ("boot reset=1 cause=0 ms=700\n"
                + LOG
                + "boot reset=1 cause=0 ms=710\n")
        window = analyze_power_log.last_session(text)
        self.assertTrue(window.startswith("boot reset=4"))
        self.assertNotIn("boot reset=1 cause=0 ms=710", window)
        summary = analyze_power_log.analyze(window)
        self.assertEqual(len(summary.boots), 1)
        self.assertEqual(len(summary.wakes), 2)

    def test_since_skips_the_old_window(self) -> None:
        prefix = "boot reset=1 cause=0 ms=700\n"
        text = prefix + LOG
        summary = analyze_power_log.analyze(text[len(prefix):])
        self.assertEqual(len(summary.boots), 2)

    def test_strict_exit_codes(self) -> None:
        script = Path(analyze_power_log.__file__).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            clean = Path(tmp) / "clean.log"
            clean.write_text(LOG, encoding="utf-8")
            ok = subprocess.run([sys.executable, str(script), str(clean), "--strict"],
                                capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)

            bad = Path(tmp) / "bad.log"
            bad.write_text(LOG + "wake reason=2 stage=3 ms=1\npanic stage=14\n",
                           encoding="utf-8")
            failed = subprocess.run([sys.executable, str(script), str(bad), "--strict"],
                                    capture_output=True, text=True)
            self.assertEqual(failed.returncode, 1)
            self.assertIn("strict: panic or refused park present", failed.stderr)
            self.assertIn("panics: 1", failed.stdout)

            short = subprocess.run(
                [sys.executable, str(script), str(clean), "--since", "99999"],
                capture_output=True, text=True)
            self.assertEqual(short.returncode, 2)

    def test_require_battery_exit_codes(self) -> None:
        script = Path(analyze_power_log.__file__).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good.log"
            good.write_text(LOG, encoding="utf-8")
            ok = subprocess.run([sys.executable, str(script), str(good), "--require-battery"],
                                capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)

            plugged = Path(tmp) / "plugged.log"
            plugged.write_text(LOG + "sample ms=240000 mv=4370 ma=0 soc=100 chg=1 usb=1\n",
                               encoding="utf-8")
            bad = subprocess.run([sys.executable, str(script), str(plugged), "--require-battery"],
                                 capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)
            self.assertIn("usb=1", bad.stderr)

            one = Path(tmp) / "one.log"
            one.write_text("sample ms=1000 mv=4300 ma=-70 soc=90 chg=0 usb=0\n",
                           encoding="utf-8")
            single = subprocess.run([sys.executable, str(script), str(one), "--require-battery"],
                                    capture_output=True, text=True)
            self.assertEqual(single.returncode, 0, single.stderr)

            empty = Path(tmp) / "empty.log"
            empty.write_text("boot reset=3 cause=0 ms=700\n", encoding="utf-8")
            none = subprocess.run([sys.executable, str(script), str(empty), "--require-battery"],
                                  capture_output=True, text=True)
            self.assertEqual(none.returncode, 1)


if __name__ == "__main__":
    unittest.main()
