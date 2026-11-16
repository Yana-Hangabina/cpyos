#!/usr/bin/env python3
"""Check that the ported features are really linked into each Plus image.

The project has been bitten repeatedly by fixes that compiled but were never
linked ("conditional compilation must prove the branch is in the image").  A
successful build alone therefore is not evidence that the web transfer page,
WebDAV, USB MSC or the protected-EPUB path made it into the shipped binaries.

This checker looks for stable byte markers in the built images (and, when a
RISC-V nm is available, function symbols in the ELF) and asserts that variants
stay separated: the reader and bring-up images must *not* carry the shell-only
services.

    python3 scripts/verify_port_features.py --artifacts ../../../../artifacts
    python3 scripts/verify_port_features.py --elf build/cpyos_metalio_plus.elf --bin build/cpyos_metalio_plus.bin

Exit status is 0 when every available image passes.  Missing images are
reported as skipped rather than failed so the script can run mid-bring-up.
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent

# Markers are literal byte sequences that only exist when the feature is linked.
SHELL_MARKERS = {
    "area-refresh guard": b"windowed scan did not return in time",
    "http file api": b"/api/list",
    "http upload api": b"/api/upload",
    "http status api": b"/api/status",
    "http settings api": b"/api/settings",
    "http diagnostics api": b"/api/diag",
    "official files page": b"/files/zh",
    "jszip asset": b"/js/jszip.min.js",
    "diagnostics page": b"/diag",
    "fonts api": b"/api/fonts",
    "official home page": b"/home/en",
    "settings page": b"Device settings</h2>",
    "http screenshot api": b"/api/screenshot.bmp",
    "http frontlight api": b"/api/frontlight",
    "browser plugin list": b"/api/plugins",
    "browser plugin runner": b"Plugin runner active",
    "browser plugin job queue": b"/api/plugin-jobs/claim",
    "browser plugin fs upload": b"/api/plugin-fs",
    "opds api": b"/api/opds/delete",
    "wifi credentials api": b"/api/wifi/delete",
    "webdav methods": b"PROPFIND",
    "webdav temp rename": b".davtmp",
    "websocket upload": b"PROGRESS:",
    "usb msc log tag": b"PLUS-MSC",
    "protected epub": b"encryption.xml",
    "pinyin data probe": b"pinyin: loaded=",
    "hyphenation data probe": b"hyphen: en=",
    "camera driver": b"GC2145",
    "bluetooth audio": b"AT+INQUIRING",
    "voice memo": b"Voice memo",
    "ai voice": b"xiaozhi",
    "4g transport": b"starting 4G data path",
    "sd sleep park": b"sdpark: read1=",
    "light-sleep handle reopen": b"light-sleep resume:",
    "audio/4g rail cycle": b"BT power reset will cut P14",
    "4g rail recovery": b"4G recovered after the BT power reset",
}

READER_MARKERS = {
    "area-refresh guard": b"displayWindow: area refresh disabled",
    "protected epub": b"encryption.xml",
    "epub engine": b"epub",
}

BSP_MARKERS = {
    "bring-up pattern": b"display bring-up pattern pushed",
    "TPS failure rails off": b"rails off, continuing without display power",
}

# Services that belong to the shell image only.
SHELL_ONLY_MARKERS = (
    b"/api/settings",
    b"/api/upload",
    b"PROPFIND",
    b"PLUS-MSC",
)

# The public shell must never link the closed-source component; the opt-in
# build (CPYOS_PLUS_JJWXC=1) is the only path that adds it.
FORBIDDEN_SYMBOLS = ("JjwxcActivity",)

# Byte markers that only exist in the private JJWXC implementation. The public
# bundle must never carry them; the opt-in build adds them deliberately.
FORBIDDEN_MARKERS = (
    b"WAP session restored",
    b"comment prefetch whole",
    b"VIP chain",
    b"jjwxc_cache_service",
)

# Symbol substrings (Itanium mangled) expected in the shell ELF.
SHELL_SYMBOLS = (
    "web_transfer5startEv",
    "settingsHandler",
    "webdav16registerHandlers",
    "ws_upload",
    "usb_msc4stopEv",
    "HalGPIO10beginFrameEv",
    "PlusTransferActivity",
    # CPYOS features that must stay linked in the public shell (the private
    # JJWXC component is checked separately and must stay absent).
    "WebBrowserActivity",
    "WeReadActivity",
    "FontExtrasActivity",
    "OpdsBookBrowserActivity",
    "SokobanGameActivity",
    "Game2048Activity",
    "ReadingStatsActivity",
    "BluetoothSettingsActivity",
)

NM_CANDIDATES = (
    "riscv32-esp-elf-nm",
    str(Path.home() / ".espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-nm"),
)


def find_nm() -> str | None:
    for candidate in NM_CANDIDATES:
        found = shutil.which(candidate) or (candidate if os.path.isfile(candidate) else None)
        if found:
            return found
    matches = sorted(glob.glob(os.path.expanduser("~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm")))
    return matches[-1] if matches else None


def check_markers(path: Path, markers: dict[str, bytes]) -> list[str]:
    data = path.read_bytes()
    return [name for name, marker in markers.items() if marker not in data]


def check_absent(path: Path, markers: tuple[bytes, ...]) -> list[str]:
    data = path.read_bytes()
    return [marker.decode("ascii", "replace") for marker in markers if marker in data]


def symbol_text(elf: Path, nm: str | None) -> str:
    if nm is None:
        return ""
    out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=False)
    return out.stdout if out.returncode == 0 else ""


def check_symbols(symbols: str, wanted: tuple[str, ...]) -> list[str]:
    return [name for name in wanted if name not in symbols]


def forbidden_symbols(symbols: str, forbidden: tuple[str, ...]) -> list[str]:
    return [name for name in forbidden if name in symbols]


def newest(artifacts: Path, pattern: str) -> Path | None:
    matches = sorted(artifacts.glob(pattern))
    return matches[-1] if matches else None


def report(label: str, path: Path | None, missing: list[str], wrong: list[str]) -> bool:
    if path is None:
        print(f"  {label:8s} skipped (image not found)")
        return True
    ok = not missing and not wrong
    status = "ok" if ok else "FAIL"
    print(f"  {label:8s} {path.name} ({path.stat().st_size:,} B) {status}")
    for name in missing:
        print(f"           missing marker: {name}")
    for name in wrong:
        print(f"           shell-only marker present: {name}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, default=None,
                        help="directory holding cpyos_metalio_plus_*_untested.bin")
    parser.add_argument("--bin", type=Path, default=None, help="built shell image")
    parser.add_argument("--reader-bin", type=Path, default=None)
    parser.add_argument("--bsp-bin", type=Path, default=None)
    parser.add_argument("--elf", type=Path, default=None, help="built shell ELF (symbol checks)")
    parser.add_argument("--allow-private", action="store_true",
                        help="dev shell built with CPYOS_PLUS_JJWXC=1; keep the symbol check for CI")
    args = parser.parse_args()

    artifacts = args.artifacts or (PORT.parents[3] / "artifacts")
    shell_bin = args.bin or (PORT / "build/cpyos_metalio_plus.bin")
    elf = args.elf or (PORT / "build/cpyos_metalio_plus.elf")
    reader_bin = args.reader_bin or (newest(artifacts, "cpyos_metalio_plus_reader_*_untested.bin") if artifacts.is_dir() else None)
    bsp_bin = args.bsp_bin or (newest(artifacts, "cpyos_metalio_plus_bsp_*_untested.bin") if artifacts.is_dir() else None)

    print("port feature presence:")
    ok = True
    shell_forbidden = ([] if args.allow_private
                       else check_absent(shell_bin, FORBIDDEN_MARKERS)) if shell_bin.is_file() else []
    ok &= report("shell", shell_bin if shell_bin.is_file() else None,
                 check_markers(shell_bin, SHELL_MARKERS) if shell_bin.is_file() else [],
                 shell_forbidden)
    ok &= report("reader", reader_bin if reader_bin and reader_bin.is_file() else None,
                 check_markers(reader_bin, READER_MARKERS) if reader_bin and reader_bin.is_file() else [],
                 check_absent(reader_bin, SHELL_ONLY_MARKERS) if reader_bin and reader_bin.is_file() else [])
    ok &= report("bsp", bsp_bin if bsp_bin and bsp_bin.is_file() else None,
                 check_markers(bsp_bin, BSP_MARKERS) if bsp_bin and bsp_bin.is_file() else [],
                 check_absent(bsp_bin, SHELL_ONLY_MARKERS) if bsp_bin and bsp_bin.is_file() else [])

    nm_bin = find_nm()
    if elf.is_file():
        symbols = symbol_text(elf, nm_bin)
        missing = check_symbols(symbols, SHELL_SYMBOLS)
        leaked = forbidden_symbols(symbols, FORBIDDEN_SYMBOLS)
        if nm_bin is None:
            print(f"  symbols  skipped (no riscv32-esp-elf-nm found; {elf.name})")
        elif (missing or leaked) and not (leaked and not missing and args.allow_private):
            ok = False
            print(f"  symbols  {elf.name} FAIL")
            for name in missing:
                print(f"           missing symbol: {name}")
            for name in leaked:
                print(f"           closed-source symbol linked: {name}")
        elif leaked:
            print(f"  symbols  {elf.name} ok (private component allowed by --allow-private)")
        else:
            print(f"  symbols  {elf.name} ok (no closed-source component)")
    else:
        print("  symbols  skipped (ELF not found)")

    print("all checked features present" if ok else "port feature check FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
