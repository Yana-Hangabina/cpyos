#!/usr/bin/env python3
"""Summarise /.crosspoint/power.log after a cable-out session.

    python3 scripts/analyze_power_log.py /tmp/power.log [--since BYTES] [--strict]

The firmware appends one line per boot, light-sleep attempt, USB presence
change and battery sample (see docs/plus-usb-less-test.md). --since isolates a
test window because the file is never truncated: pass the size `cat` reported
before unplugging. --strict exits 1 on a panic or a refused park, so the same
command can gate a bench run.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

_RESET_NAMES = {
    0: "unknown",
    1: "poweron",
    2: "ext",
    3: "sw",
    4: "panic",
    5: "int_wdt",
    6: "task_wdt",
    7: "wdt",
    8: "brownout",
    9: "sdio",
    10: "usb",
    11: "jtag",
    12: "efuse",
    13: "pwr_glitch",
    14: "cpu_lockup",
}
_WAKE_NAMES = {0: "timer", 1: "power", 2: "failed"}


def _fields(line: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for token in line.split():
        key, sep, value = token.partition("=")
        if sep:
            out[key] = value
    return out


def _int(fields: dict[str, str], key: str) -> int | None:
    try:
        return int(fields[key])
    except (KeyError, ValueError):
        return None


@dataclass
class Summary:
    boots: list[dict] = field(default_factory=list)
    wakes: list[dict] = field(default_factory=list)
    usb: list[dict] = field(default_factory=list)
    samples: list[dict] = field(default_factory=list)
    previous_wakes: list[dict] = field(default_factory=list)
    panics: list[str] = field(default_factory=list)

    def failed_wakes(self) -> list[dict]:
        return [w for w in self.wakes if w["reason"] == 2]

    def power_wakes(self) -> list[dict]:
        return [w for w in self.wakes if w["reason"] == 1]

    def timer_wakes(self) -> list[dict]:
        return [w for w in self.wakes if w["reason"] == 0]


def last_session(text: str) -> str:
    """Everything between the second-to-last and the last boot line.

    Replugging USB resets the SoC and appends a boot line, so the cable-out
    window is exactly the segment before that final boot.
    """
    lines = text.splitlines()
    boots = [index for index, line in enumerate(lines) if line.strip().startswith("boot ")]
    if len(boots) < 2:
        return text
    return "\n".join(lines[boots[-2]:boots[-1]])


def analyze(text: str) -> Summary:
    summary = Summary()
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("boot "):
            fields = _fields(line)
            summary.boots.append(
                {"reset": _int(fields, "reset"), "cause": _int(fields, "cause"),
                 "ms": _int(fields, "ms"), "stage": _int(fields, "stage")})
        elif line.startswith("previous-wake "):
            fields = _fields(line)
            summary.previous_wakes.append(
                {"reason": _int(fields, "reason"), "stage": _int(fields, "stage")})
        elif line.startswith("wake "):
            fields = _fields(line)
            summary.wakes.append(
                {"reason": _int(fields, "reason"), "stage": _int(fields, "stage"),
                 "ms": _int(fields, "ms"), "mv": _int(fields, "mv"),
                 "ma": _int(fields, "ma"), "soc": _int(fields, "soc"),
                 "usb": _int(fields, "usb")})
        elif line.startswith("usb="):
            fields = _fields(line)
            summary.usb.append({"usb": _int(fields, "usb"), "ms": _int(fields, "ms")})
        elif line.startswith("sample "):
            fields = _fields(line)
            summary.samples.append(
                {"ms": _int(fields, "ms"), "mv": _int(fields, "mv"),
                 "ma": _int(fields, "ma"), "soc": _int(fields, "soc"),
                 "chg": _int(fields, "chg"), "usb": _int(fields, "usb"),
                 "wifi": _int(fields, "wifi")})
        elif line.startswith("panic"):
            summary.panics.append(line)
    return summary


def battery_curve(samples: list[dict]) -> dict:
    usable = [s for s in samples if s["ma"] is not None and s["ms"] is not None]
    if len(usable) < 2:
        return {}
    first, last = usable[0], usable[-1]
    minutes = (last["ms"] - first["ms"]) / 60000.0
    energy = 0.0
    for a, b in zip(usable, usable[1:]):
        hours = (b["ms"] - a["ms"]) / 3600000.0
        energy += (a["ma"] + b["ma"]) / 2.0 * hours
    mean_ma = energy / (minutes / 60.0) if minutes > 0 else 0.0
    return {
        "count": len(usable),
        "minutes": minutes,
        "mean_ma": mean_ma,
        "charge_mah": energy,
        "mv_delta": (last["mv"] - first["mv"]) if None not in (first["mv"], last["mv"]) else None,
        "soc_delta": (last["soc"] - first["soc"]) if None not in (first["soc"], last["soc"]) else None,
        "all_usb0": all(s["usb"] == 0 for s in usable),
    }


def report(text: str) -> str:
    s = analyze(text)
    lines: list[str] = []
    lines.append(f"boots: {len(s.boots)}")
    for boot in s.boots:
        reset = _RESET_NAMES.get(boot["reset"], str(boot["reset"]))
        stage = "" if boot["stage"] is None else f" stage={boot['stage']}"
        lines.append(f"  boot reset={reset}{stage} ms={boot['ms']}")
    lines.append(f"usb transitions: {len(s.usb)}"
                 + (f" (last usb={s.usb[-1]['usb']})" if s.usb else ""))
    for entry in s.previous_wakes:
        lines.append(f"  previous-wake reason={_WAKE_NAMES.get(entry['reason'], entry['reason'])}"
                     f" stage={entry['stage']}")
    power = s.power_wakes()
    timer = s.timer_wakes()
    failed = s.failed_wakes()
    lines.append(f"wake attempts: {len(s.wakes)} (power={len(power)}, timer={len(timer)},"
                 f" failed={len(failed)})")
    for wake in s.wakes:
        lines.append(f"  wake reason={_WAKE_NAMES.get(wake['reason'], wake['reason'])}"
                     f" stage={wake['stage']} ms={wake['ms']}")
    lines.append(f"panics: {len(s.panics)}")
    for panic in s.panics:
        lines.append(f"  {panic}")
    curve = battery_curve(s.samples)
    if curve:
        lines.append(f"battery samples: {curve['count']} over {curve['minutes']:.1f} min"
                     f" (all usb=0: {'yes' if curve['all_usb0'] else 'no'})")
        lines.append(f"  mean current {curve['mean_ma']:.1f} mA,"
                     f" charge {curve['charge_mah']:.1f} mAh")
        if curve["mv_delta"] is not None:
            lines.append(f"  mV {curve['mv_delta']:+d}, SOC {curve['soc_delta']:+d}%")
    else:
        lines.append(f"battery samples: {len(s.samples)} (need two to compute a curve)")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--since", type=int, default=0,
                        help="ignore the first N bytes (a test-window marker)")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 on a panic or a refused park")
    parser.add_argument("--last-session", action="store_true",
                        help="only the segment before the final boot line")
    parser.add_argument("--require-battery", action="store_true",
                        help="exit 1 unless two samples exist and every one has usb=0")
    args = parser.parse_args()
    data = args.log.read_bytes()
    if args.since > len(data):
        print(f"{args.log}: only {len(data)} bytes, --since {args.since}", file=sys.stderr)
        return 2
    text = data[args.since:].decode("utf-8", "replace")
    if args.last_session:
        text = last_session(text)
    print(report(text))
    summary = analyze(text)
    if args.strict and (summary.panics or summary.failed_wakes()):
        print("strict: panic or refused park present", file=sys.stderr)
        return 1
    if args.require_battery:
        samples = [s for s in summary.samples if s["usb"] is not None]
        if not samples or any(s["usb"] != 0 for s in samples):
            print("require-battery: need at least one sample with usb=0 and none with usb=1",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
