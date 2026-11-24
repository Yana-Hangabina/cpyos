#!/usr/bin/env python3
"""Render-path code must not write internal flash, and the radio teardown must
prove it stays off flash.

Why: ActivityManager invokes onEnter()/render() on the render task whose stack is
in PSRAM, and any NVS/esp_partition/esp_ota write from there trips IDF's
cache-disabled stack guard (AGENTS 85.13).  This test is a static guard on the
reader's render path and on NetworkStartup::releaseRadioDeep(), where the fix is
the *ordering* of esp_wifi_set_storage(WIFI_STORAGE_RAM) before WiFi.mode().
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
READER = ROOT / "src/activities/reader/PdfReaderActivity.cpp"
NET = ROOT / "src/NetworkStartup.cpp"

FLASH_CALL = re.compile(r"saveToFile\s*\(|Preferences|nvs_[a-z_]+\(|esp_partition_(write|erase)|esp_ota_")
DEF_RE = re.compile(r"^[A-Za-z_][\w:<>,&\* ]*\s+(?:[\w:]+::)?[~\w]+\s*\([^;]*$")
RENDER_FNS = {
    "PdfReaderActivity::renderPage", "PdfReaderActivity::renderBook", "PdfReaderActivity::renderReflow",
    "PdfReaderActivity::renderToolbarOverlay", "PdfReaderActivity::drawStatusBar",
    "PdfReaderActivity::paintRenderFailed", "PdfReaderActivity::ensureFullCache",
    "PdfReaderActivity::buildReflow", "PdfReaderActivity::drawCacheProgress",
}


def strip_comments(text):
    """Comments name the very calls this test looks for ('Arduino's WiFi.mode()
    persists...'), so scanning raw text reports the explanation as the offence."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def functions(text):
    lines = strip_comments(text).splitlines()
    out, i = {}, 0
    while i < len(lines):
        line = lines[i]
        # Class members carry `::`; NetworkStartup's teardown is a free function,
        # so both shapes are collected (the ordering check below needs the free one).
        if DEF_RE.match(line) and not line.rstrip().endswith(";"):
            balance = 0
            body, started = [], False
            j = i
            while j < len(lines):
                body.append(lines[j])
                balance += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    started = True
                if started and balance <= 0:
                    break
                j += 1
            name = re.sub(r".*?([\w:]+::[~\w]+|[~\w]+)\s*\(.*", r"\1", line.strip())
            out[name] = "\n".join(body)
            i = j
        i += 1
    return out


def main():
    failures = []
    reader_fns = functions(READER.read_text(errors="replace"))
    checked = 0
    for name in sorted(RENDER_FNS):
        body = reader_fns.get(name)
        if body is None:
            failures.append(f"{name}: function not found (renamed? update this list)")
            continue
        checked += 1
        for match in FLASH_CALL.finditer(body):
            failures.append(f"{name}: flash-writing call '{match.group(0)}' on the render path")
    if checked < 6:
        failures.append(f"only {checked} render functions found -- the scanner is not looking at the right code")

    # The teardown's safety is ordering: RAM storage must be set before any mode change.
    net = NET.read_text(errors="replace")
    fn = functions(net).get("releaseRadioDeep") or ""
    if not fn:
        failures.append("releaseRadioDeep() not found in NetworkStartup.cpp")
    else:
        storage = fn.find("esp_wifi_set_storage(WIFI_STORAGE_RAM)")
        mode = fn.find("WiFi.mode(")
        if storage < 0:
            failures.append("releaseRadioDeep() no longer pins WiFi storage to RAM")
        elif mode >= 0 and storage > mode:
            failures.append("releaseRadioDeep() changes WiFi.mode before switching storage to RAM")

    for f in failures:
        print("FAIL", f, file=sys.stderr)
    print(f"render-path flash guard: {checked} functions checked, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
