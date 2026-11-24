#!/usr/bin/env python3
"""Inventory the kiikoread (read-pico-reader) application tree by disposition.

`inventory_kiikoread.py` answers "what licence is each reference file under?".
This script answers the complementary question: "what did this port do with
each reference subsystem?"  Every top-level area, component and main-root file
of the reference checkout must be classified, so a newer rc cannot silently add
an unexamined subsystem.

    python3 scripts/inventory_reference_app.py --tree /path/to/read-pico-reader
    python3 scripts/inventory_reference_app.py --tree ... --markdown ../../docs/engineering/metalio-eink4-plus-rc110-app.md
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
REPO = PORT.parents[1]
CODE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".S", ".py", ".sh", ".js")

# status: ported | reference | replaced | not-ported | not-applicable | build
DISPOSITIONS: dict[str, tuple[str, str]] = {
    # boards
    "boards/pico": ("reference", "Read Pico board bring-up; CPYOS has its own board profile and the Plus uses the vendor BSP."),
    "boards/metalio47": ("reference", "Plus board reference material; the port imports the vendor CloudZao BSP instead of these files."),
    # components
    "components/ble_page_turner": ("reference", "CPYOS implements its own NimBLE HID central and report decoding."),
    "components/continuous_du": ("reference", "Fast-refresh/DU policy; the port maps FAST to the differential GL16 waveform."),
    "components/cst836u": ("reference", "Touch controller reference; the port uses the vendor FT6336U driver."),
    "components/e0470_epaper_waveform": ("ported", "Waveform/gamma work ported into freeink-sdk with Apache notices (Read Pico display path)."),
    "components/e0470_page_turn": ("ported", "Water-ripple page-turn engine ported into freeink-sdk (Read Pico only on this port)."),
    "components/epdiy": ("reference", "Upstream epdiy; CPYOS vendors its own patched copy via freeink-sdk."),
    "components/espressif__esp_tinyusb": ("not-applicable", "Managed component; the port depends on esp_tinyusb directly."),
    "components/fca9555": ("reference", "I/O expander reference; the port uses the vendor pca9555 driver."),
    "components/jpegdec": ("replaced", "CPYOS uses its own JPEGDEC wrapper and image pipeline."),
    "components/kiiko_board": ("reference", "Board abstraction reference."),
    "components/kiiko_locale": ("reference", "Locale reference; CPYOS uses its own I18n and generated packs."),
    "components/metalio_hw": ("reference", "Vendor-adjacent HW helpers; the port has its own HAL plus the vendor BSP."),
    "components/pwm_audio": ("not-ported", "Audio output is not implemented; the speaker needs the external I2S master/codec."),
    "components/read_pico": ("reference", "Board/SD bring-up; the port implements cpyos_sd.c independently."),
    "components/read_pico_pmu": ("reference", "Power/PMU policy analysed in §2.7; no code is copied."),
    "components/read_pico_search": ("replaced", "Search is provided by the CPYOS library index and pinyin matcher."),
    "components/read_pico_transfer": ("replaced", "Replaced by the native HTTP/WebDAV/Calibre servers and PlusTransferActivity."),
    "components/read_pico_webp": ("not-ported", "WebP decoding is out of scope; CPYOS decodes JPEG/PNG/GIF."),
    "components/sc7a20h": ("reference", "IMU reference; the port uses the freeink Imu abstraction on the same part."),
    "components/sy7636a": ("reference", "PMIC reference; the port drives TPS65185 through the vendor BSP."),
    # main directories
    "main/app": ("replaced", "Application loop replaced by ActivityManager."),
    "main/apps": ("replaced", "Reader/UI applications replaced by the shared CPYOS activities."),
    "main/book": ("replaced", "Book engine replaced by CPYOS EPUB/TXT/FB2/XTC/CBZ."),
    "main/font": ("replaced", "Fonts replaced by EpdFont/FreeType/SdCardFontSystem."),
    "main/ui": ("replaced", "UI replaced by GfxRenderer, CPY theme and FreeInkUI."),
    "main/assets": ("not-applicable", "Reference UI assets and fonts."),
    "main/factory": ("reference", "Factory VCOM/setup flow; the port does not ship a factory flow."),
    # main root files
    "main/anti_fade.c": ("reference", "Anti-fade/refresh policy reference."),
    "main/anti_fade.h": ("reference", "Anti-fade/refresh policy reference."),
    "main/auto_lock.h": ("replaced", "Auto-lock replaced by the CPYOS auto-standby setting."),
    "main/boot_state.c": ("replaced", "Boot/resume state replaced by CPYOS settings and reader resume."),
    "main/boot_state.h": ("replaced", "Boot/resume state replaced by CPYOS settings and reader resume."),
    "main/CMakeLists.txt": ("build", "Reference build file."),
    "main/display_main_waveform.h": ("reference", "Display waveform reference (see §2.5)."),
    "main/display_reader_waveform.h": ("reference", "Display waveform reference (see §2.5)."),
    "main/display_waveform_smooth.h": ("reference", "Display waveform reference."),
    "main/display.h": ("reference", "Display API reference."),
    "main/factory_reset.c": ("reference", "Factory reset reference; not shipped on the port."),
    "main/factory_reset.h": ("reference", "Factory reset reference; not shipped on the port."),
    "main/idf_component.yml": ("build", "Reference component manifest."),
    "main/kiiko_board_config.h": ("reference", "Board configuration reference."),
    "main/lock_pin.c": ("not-applicable", "Lock-screen PIN; CPYOS standby has no PIN entry."),
    "main/lock_pin.h": ("not-applicable", "Lock-screen PIN; CPYOS standby has no PIN entry."),
    "main/lock_pin_flow.c": ("not-applicable", "Lock-screen PIN flow."),
    "main/lock_pin_flow.h": ("not-applicable", "Lock-screen PIN flow."),
    "main/lock_screen.c": ("not-applicable", "Reference lock-screen UI; the port uses ActivityManager standby."),
    "main/lock_screen.h": ("not-applicable", "Reference lock-screen UI."),
    "main/metalio_crash.lf": ("reference", "Crash log asset reference."),
    "main/ota_online.h": ("reference", "OTA policy reference; the port has no OTA (single slot)."),
    "main/ota_release.c": ("reference", "OTA policy reference; the port has no OTA (single slot)."),
    "main/ota_update.c": ("reference", "OTA policy reference; the port has no OTA (single slot)."),
    "main/ota_update.h": ("reference", "OTA policy reference; the port has no OTA (single slot)."),
    "main/settings.c": ("replaced", "Replaced by CPYOS CrossPointSettings (JSON on SD)."),
    "main/settings.h": ("replaced", "Replaced by CPYOS CrossPointSettings."),
    "main/setup_state.c": ("replaced", "Onboarding state replaced by CPYOS settings/setup flow."),
    "main/setup_state.h": ("replaced", "Onboarding state replaced by CPYOS settings/setup flow."),
    "main/sleep.h": ("reference", "Sleep API reference; the sequence is documented in §2.7."),
    "main/time_service.c": ("replaced", "Replaced by HalClock (PCF8563 + SNTP)."),
    "main/time_service.h": ("replaced", "Replaced by HalClock (PCF8563 + SNTP)."),
    # top-level areas
    "tools": ("reference", "Reference host tests; CPYOS keeps its own suites."),
    "assets": ("not-applicable", "Reference marketing/product assets."),
    "docs": ("reference", "Board/power/display behaviour documentation."),
    "cmake": ("build", "Reference build helpers."),
    "flash": ("build", "Reference flashing assets."),
}


def default_tree() -> Path | None:
    for candidate in (REPO.parents[1] / "repos" / "kiikoread-rc110",
                      REPO.parents[1] / ".scratch" / "metalio-research" / "kiikoread-rc110",
                      REPO / ".scratch" / "metalio-research" / "kiikoread-rc110"):
        if (candidate / "main").is_dir():
            return candidate
    return None


def code_files(path: Path) -> int:
    if path.is_file():
        return 1 if path.suffix in CODE_SUFFIXES else 0
    return sum(1 for item in path.rglob("*") if item.is_file() and item.suffix in CODE_SUFFIXES)


def collect(tree: Path) -> tuple[list[dict], list[str]]:
    rows: list[dict] = []
    unknown: list[str] = []

    def add(relative: str, path: Path) -> None:
        disposition = DISPOSITIONS.get(relative)
        if disposition is None:
            unknown.append(relative)
            return
        status, note = disposition
        rows.append({
            "entry": relative + ("/" if path.is_dir() else ""),
            "kind": "dir" if path.is_dir() else "file",
            "code_files": code_files(path),
            "status": status,
            "note": note,
        })

    for area in ("main", "components", "boards"):
        for child in sorted((tree / area).iterdir()):
            if child.name.startswith("."):
                continue
            relative = f"{area}/{child.name}"
            if area == "main" and child.is_dir():
                add(relative, child)
            elif child.is_file():
                add(relative, child)
            else:
                add(relative, child)

    for area in ("tools", "assets", "docs", "cmake", "flash"):
        path = tree / area
        if path.is_dir():
            add(area, path)

    return rows, unknown


def render_markdown(rows: list[dict]) -> str:
    lines = [
        "# kiikoread application-tree dispositions (Metalio E-INK4 Plus port)",
        "",
        "Generated by `ports/metalio_plus/scripts/inventory_reference_app.py` from an rc110 checkout.",
        "Code-file counts include `.c/.cc/.cpp/.h/.hpp/.S/.py/.sh/.js`; licence facts live in",
        "[`metalio-eink4-plus-rc110-files.md`](metalio-eink4-plus-rc110-files.md).",
        "",
        "| Area | Kind | Code files | Disposition | Why |",
        "|---|---|---:|---|---|",
    ]
    for row in rows:
        lines.append(f"| `{row['entry']}` | {row['kind']} | {row['code_files']} | "
                     f"**{row['status']}** | {row['note']} |")
    lines.append("")
    lines.append("| Disposition | Areas | Code files |")
    lines.append("|---|---:|---:|")
    for status in ("ported", "reference", "replaced", "not-ported", "not-applicable", "build"):
        selected = [row for row in rows if row["status"] == status]
        if selected:
            lines.append(f"| {status} | {len(selected)} | {sum(r['code_files'] for r in selected)} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tree", type=Path, default=None)
    parser.add_argument("--markdown", type=Path, default=None)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    tree = args.tree or default_tree()
    if tree is None or not (tree / "main").is_dir():
        raise SystemExit("reference tree not found; pass --tree /path/to/read-pico-reader")
    rows, unknown = collect(tree)
    if unknown:
        raise SystemExit("unclassified reference entries (update DISPOSITIONS): " + ", ".join(unknown))

    if args.json:
        print(json.dumps({"tree": str(tree), "rows": rows}, ensure_ascii=False, indent=2))
    else:
        for row in rows:
            print(f"{row['entry']:44s} {row['status']:14s} code={row['code_files']}")
        print(f"classified {len(rows)} entries")

    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(render_markdown(rows), encoding="utf-8")
        print(f"wrote {args.markdown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())