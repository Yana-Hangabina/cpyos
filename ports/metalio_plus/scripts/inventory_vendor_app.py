#!/usr/bin/env python3
"""Inventory every top-level area of the vendor firmware and record its disposition.

The E-INK4 Plus reference firmware is much larger than the BSP subset copied into
this port.  To keep the "full support" claim honest, every top-level entry of
`vendor/main` must be classified: ported, replaced by a CPYOS equivalent,
analysed as a reference, deliberately not ported, or not applicable.  A new
directory/file in a newer vendor drop fails this check until it is classified.

    python3 scripts/inventory_vendor_app.py --tree /path/to/vendor
    python3 scripts/inventory_vendor_app.py --tree ... --markdown ../../docs/engineering/metalio-eink4-plus-vendor-app.md
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
REPO = PORT.parents[1]

CODE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")

# status: ported | reference | replaced | not-ported | not-applicable | build
DISPOSITIONS: dict[str, tuple[str, str]] = {
    "boards": ("ported",
               "Board BSP subset lives in components/cpyos_metalio_bsp (display, TPS/TCA, touch, SD, "
               "battery/RTC/charger). The application-layer power policy is analysed in §2.7 but not ported."),
    "display": ("replaced",
                "Vendor LVGL/A2UI/epd-font UI stack is replaced by CPYOS GfxRenderer/Activity/EpdFont; "
                "the vendor board display path is the source of the vendored BSP."),
    "audio": ("ported",
              "BH1098G mode-1 UART/I2S path in components/cpyos_metalio_bsp/plus_audio.c, "
              "Bluetooth audio page, voice memo and the push-to-talk xiaozhi client in components/cpyos_app; "
              "no vendor AFE/wake word on the S31."),
    "reader": ("replaced",
               "Vendor reader/JPEG pipeline is replaced by the CPYOS book engine (EPUB/TXT/FB2/XTC/CBZ)."),
    "protocols": ("not-applicable",
                  "Vendor cloud/assistant protocol helpers are not reused; CPYOS has its own HTTP/TLS/"
                  "WebDAV stacks and reimplements the xiaozhi WebSocket v1/v2/v3 wire format in "
                  "components/cpyos_app/plus_voice_client.cpp."),
    "workbuddy": ("not-applicable",
                  "Vendor cloud AI assistant application; no service integration in this port."),
    "assets": ("not-applicable", "Vendor UI/assistant asset bundle."),
    "factory-test-assets": ("not-applicable", "Vendor factory test fixtures."),
    "xingzhi-assets": ("not-applicable", "Vendor asset directory (no code)."),
    "api_endpoints.h": ("not-applicable", "Vendor cloud endpoint table."),
    "api_http.h": ("not-applicable", "Vendor cloud HTTP helpers."),
    "app_main.cc": ("replaced", "Entry point replaced by CPYOS main/ActivityManager."),
    "application.cc": ("replaced", "Vendor application framework replaced by ActivityManager."),
    "application.h": ("replaced", "Vendor application framework replaced by ActivityManager."),
    "application_tasks.cc": ("replaced", "Vendor task wiring replaced by CPYOS jobs/tasks."),
    "ask_ai_boot_stubs.cc": ("not-applicable", "Vendor cloud AI stubs."),
    "assets.cc": ("not-applicable", "Vendor asset bundle implementation."),
    "assets.h": ("not-applicable", "Vendor asset bundle interface."),
    "battery_uv_lockout.h": ("ported",
                             "3.1 V software guard in the shell (30 s poll, 10 s grace, P1.3 pulse train); "
                             "the CX25601N BATFET register itself stays read-only until cell parameters are measured."),
    "chat_cloud.h": ("not-applicable", "Vendor cloud chat."),
    "chat_memory.h": ("not-applicable", "Vendor cloud chat memory."),
    "cloudzao_endpoints.c": ("not-applicable", "Vendor cloud endpoints."),
    "cloudzao_endpoints.h": ("not-applicable", "Vendor cloud endpoints."),
    "device_state.h": ("replaced", "Vendor device state replaced by CrossPointSettings/settings.json."),
    "device_state_event.cc": ("replaced", "Vendor state events replaced by CPYOS settings/activity flow."),
    "device_state_event.h": ("replaced", "Vendor state events replaced by CPYOS settings/activity flow."),
    "device_wifi_location.h": ("not-applicable", "Vendor Wi-Fi geolocation; not used."),
    "firmware_memory.h": ("reference", "Vendor memory constants; CPYOS uses its own heap policy."),
    "haptic_feedback.cc": ("ported",
                           "Motor pulse semantics in HalGPIO on TCA P0.3; pulse widths now match the vendor "
                           "constants 35/50/70 ms (the vendor's 70 % soft-PWM is not ported)."),
    "haptic_feedback.h": ("ported", "See haptic_feedback.cc: HalGPIO pulse on TCA P0.3."),
    "mcp_server.cc": ("not-applicable", "Vendor MCP service; depends on the vendor AI cloud."),
    "mcp_server.h": ("not-applicable", "Vendor MCP service; depends on the vendor AI cloud."),
    "model_path.h": ("not-applicable", "Vendor AI model paths; the S31 build has no AFE/wake word."),
    "ota.cc": ("not-ported",
               "Vendor cloud OTA needs plus_ota_key.local and burner.cloudzao.com; CPYOS OTA stays "
               "disabled (no Plus release channel). Device-side SD image flashing writes the inactive "
               "app slot through the shared SdFirmwareUpdateActivity/flash path."),
    "ota.h": ("not-ported", "See ota.cc: vendor cloud OTA only; CPYOS flashes images from SD."),
    "ota_ignore.cc": ("not-ported", "Vendor OTA helper; no consumer in CPYOS."),
    "ota_ignore.h": ("not-ported", "Vendor OTA helper; no consumer in CPYOS."),
    "settings.cc": ("replaced", "Vendor settings replaced by CPYOS CrossPointSettings (JSON on SD)."),
    "settings.h": ("replaced", "Vendor settings replaced by CPYOS CrossPointSettings."),
    "system_info.cc": ("replaced", "Vendor system info replaced by HalSystem."),
    "system_info.h": ("replaced", "Vendor system info replaced by HalSystem."),
    "yunxin_text_format.h": ("not-applicable", "Vendor cloud text formatting."),
    "yunxin_text_inbox.h": ("not-applicable", "Vendor cloud text inbox."),
    "CMakeLists.txt": ("build", "Vendor build file."),
    "Kconfig.projbuild": ("build", "Vendor build configuration."),
    "idf_component.yml": ("build", "Vendor component manifest."),
}


def default_tree() -> Path | None:
    for candidate in (REPO.parents[1] / "repos" / "metalio-e-ink4-plus",
                      REPO.parents[1] / ".scratch" / "metalio-research" / "vendor",
                      REPO / ".scratch" / "metalio-research" / "vendor"):
        if (candidate / "main").is_dir():
            return candidate
    return None


def code_files(path: Path) -> int:
    if path.is_file():
        return 1 if path.suffix in CODE_SUFFIXES else 0
    return sum(1 for item in path.rglob("*") if item.is_file() and item.suffix in CODE_SUFFIXES)


def collect(tree: Path) -> tuple[list[dict], list[str]]:
    main = tree / "main"
    rows: list[dict] = []
    unknown: list[str] = []
    for entry in sorted(main.iterdir()):
        disposition = DISPOSITIONS.get(entry.name)
        if disposition is None:
            unknown.append(entry.name)
            continue
        status, note = disposition
        rows.append({
            "entry": entry.name + ("/" if entry.is_dir() else ""),
            "kind": "dir" if entry.is_dir() else "file",
            "code_files": code_files(entry),
            "status": status,
            "note": note,
        })
    return rows, unknown


def render_markdown(rows: list[dict], tree: Path | None) -> str:
    origin = f"the `{tree.name}` checkout" if tree else "(tree not specified)"
    lines = [
        "# Vendor application-layer inventory (Metalio E-INK4 Plus)",
        "",
        f"Generated by `ports/metalio_plus/scripts/inventory_vendor_app.py` from {origin}.",
        "Counts are code files (`.c/.cc/.cpp/.h/.hpp`) under each top-level entry of `vendor/main`.",
        "A new top-level entry must be classified here before it can be ignored.",
        "",
        "| Entry | Kind | Code files | Disposition | Notes |",
        "|---|---|---:|---|---|",
    ]
    for row in rows:
        lines.append(f"| `{row['entry']}` | {row['kind']} | {row['code_files']} | "
                     f"**{row['status']}** | {row['note']} |")
    lines.append("")
    lines.append("| Disposition | Entries | Code files |")
    lines.append("|---|---:|---:|")
    order = ["ported", "reference", "replaced", "not-ported", "not-applicable", "build"]
    for status in order:
        selected = [row for row in rows if row["status"] == status]
        if selected:
            lines.append(f"| {status} | {len(selected)} | {sum(r['code_files'] for r in selected)} |")
    lines.append("")
    lines.append("Dispositions: **ported** (code carried into this port), **reference** (read for policy/values, "
                 "no code copied), **replaced** (CPYOS has its own implementation), **not-ported** (deliberately "
                 "out of scope, with the blocker documented), **not-applicable** (vendor cloud/UI/asset stack), "
                 "**build** (vendor build files).")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tree", type=Path, default=None, help="vendor firmware checkout (contains main/)")
    parser.add_argument("--markdown", type=Path, default=None, help="write a markdown appendix")
    parser.add_argument("--json", action="store_true", help="print machine-readable rows")
    args = parser.parse_args()

    tree = args.tree or default_tree()
    if tree is None or not (tree / "main").is_dir():
        raise SystemExit("vendor tree not found; pass --tree /path/to/vendor")
    rows, unknown = collect(tree)
    if unknown:
        raise SystemExit("unclassified vendor entries (update DISPOSITIONS): " + ", ".join(unknown))

    if args.json:
        print(json.dumps({"tree": str(tree), "rows": rows}, ensure_ascii=False, indent=2))
    else:
        for row in rows:
            print(f"{row['entry']:28s} {row['status']:14s} code={row['code_files']}")
        print(f"classified {len(rows)} entries")

    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(render_markdown(rows, tree), encoding="utf-8")
        print(f"wrote {args.markdown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
