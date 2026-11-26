#!/usr/bin/env python3
"""Flash a built Metalio E-INK4 Plus image safely.

Two layouts exist and they are NOT interchangeable:

vendor (default, matches a stock machine)
    CloudZao v2.0.74 has two 5 MiB app slots: ota_0 at 0x80000 and ota_1 at
    0x580000, plus model/resources/font_data spiffs partitions.  Only an app
    image that fits 5 MiB can be written here.  The bring-up and reader images
    fit; the full shell does not.  This mode never writes data packs,
    bootloader, partition table, otadata, nvs, coredump or any vendor data.

port (full install, destructive)
    The CPYOS single-slot layout in partitions.csv: one 14.375 MiB app slot at
    0x10000 and a 1.5 MiB spiffs at 0xE70000.  A stock machine does NOT have
    this layout.  Using it requires writing the partition table, so the script
    requires an explicit --full-install and a full 16 MiB --backup.

Guards:
  * the app descriptor must say `cpyos_metalio_plus` and carry an `untested`
    version - a Read Pico or release image is refused before any write;
  * the app must fit the selected slot (5 MiB on vendor, 14.375 MiB on port);
  * a preflight reads the real partition table at 0x8000 and refuses a
    mismatch instead of writing into the wrong partition;
  * port layout pack ranges are checked for overlap and the reserved gap at
    spiffs +0x80000 is excluded;
  * after writing, every segment is verified with `esptool verify_flash` and
    the run only succeeds when its hash matches.

Usage:
    # safe first hardware test: bring-up image in the stock layout
    python3 scripts/flash_metalio_plus.py \
        --firmware ../../../artifacts/cpyos_metalio_plus_bringup_20261107_untested.bin \
        --layout vendor --slot both --backup /tmp/plus-16m.bin

    # only inspect the plan
    python3 scripts/flash_metalio_plus.py --layout vendor --dry-run

    # destructive CPYOS full install (after a backup; replaces the stock table)
    python3 scripts/flash_metalio_plus.py --layout port --full-install \
        --backup /tmp/plus-16m.bin

The backup flag runs before any write and refuses to overwrite an existing file.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PORT_DIR = Path(__file__).resolve().parents[1]
REPO = PORT_DIR.parents[1]
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 0x20
UNTESTED_PROJECT = "cpyos_metalio_plus"
SPIFFS_RESERVED = 0x80000  # 12 factory bytes; never write across them
IMAGE_SIZE = 0x1000000
PORT_LAYOUT = "port"
PORT_OTA_LAYOUT = "port-ota"
VENDOR_LAYOUT = "vendor"
PORT_PARTITIONS_CSV = PORT_DIR / "partitions.csv"
PORT_OTA_PARTITIONS_CSV = PORT_DIR / "partitions_plus_ota.csv"
VENDOR_PARTITIONS_CSV = PORT_DIR / "partitions_vendor.csv"
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_READ = 0x1000


def parse_size(text: str) -> int:
    text = text.strip()
    if not text:
        raise ValueError("empty size")
    suffix = text[-1].upper()
    if suffix == "K":
        return int(text[:-1], 10) * 1024
    if suffix == "M":
        return int(text[:-1], 10) * 1024 * 1024
    return int(text, 0)


def parse_partitions(csv_path: Path) -> dict[str, tuple[int, int]]:
    parts: dict[str, tuple[int, int]] = {}
    for line in csv_path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            continue
        name = fields[0]
        try:
            parts[name] = (int(fields[3], 0), parse_size(fields[4]))
        except ValueError:
            continue
    return parts


def parse_partition_rows(csv_path: Path) -> list[dict]:
    rows: list[dict] = []
    for line in csv_path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            continue
        try:
            rows.append({
                "name": fields[0],
                "type": fields[1],
                "subtype": fields[2],
                "offset": int(fields[3], 0),
                "size": parse_size(fields[4]),
            })
        except ValueError:
            continue
    return rows


def read_partition_table_binary(data: bytes) -> list[dict]:
    """Decode an ESP-IDF partition table image (stops at the MD5 entry)."""
    rows: list[dict] = []
    off = 0
    while off + 32 <= len(data):
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", data, off)
        if magic != 0x50AA:
            break
        label = data[off + 12:off + 28].split(b"\0", 1)[0].decode("utf-8", "replace")
        flags = struct.unpack_from("<I", data, off + 28)[0]
        rows.append({"name": label, "type": ptype, "subtype": subtype,
                     "offset": offset, "size": size, "flags": flags})
        off += 32
    return rows


def tables_match(expected: list[dict], actual: list[dict]) -> bool:
    if not actual:
        return False
    seen = {row["name"]: row for row in actual}
    for row in expected:
        got = seen.get(row["name"])
        if got is None:
            return False
        if got["offset"] != row["offset"] or got["size"] != row["size"]:
            return False
    return True


def describe_table(rows: list[dict]) -> str:
    if not rows:
        return "<unreadable>"
    return ", ".join(f"{r['name']}@0x{r['offset']:X}+0x{r['size']:X}" for r in rows)


def read_constant(header: Path, name: str) -> int:
    text = header.read_text(encoding="utf-8")
    match = re.search(rf"\b{name}\b\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
    if not match:
        raise SystemExit(f"{header}: cannot read {name}")
    return int(match.group(1), 0)


def describe(app: Path) -> tuple[str, str]:
    data = app.read_bytes()
    off = APP_DESC_OFFSET
    if len(data) < off + 0x80 or struct.unpack_from("<I", data, off)[0] != APP_DESC_MAGIC:
        raise SystemExit(f"{app}: no esp_app_desc (not a firmware image)")
    version = data[off + 0x10:off + 0x30].split(b"\0", 1)[0].decode("utf-8", "replace")
    project = data[off + 0x30:off + 0x50].split(b"\0", 1)[0].decode("utf-8", "replace")
    return project, version


def check_ranges(ranges: list[tuple[str, int, int]], spiffs_base: int, spiffs_size: int) -> None:
    ordered = sorted(ranges, key=lambda item: item[1])
    for name, start, size in ordered:
        if start < spiffs_base or start + size > spiffs_base + spiffs_size:
            raise SystemExit(f"{name}: 0x{start:X}+0x{size:X} leaves the spiffs partition")
        if start == spiffs_base + SPIFFS_RESERVED or (
                start < spiffs_base + SPIFFS_RESERVED < start + size):
            raise SystemExit(f"{name}: overlaps the reserved bytes at spiffs+0x{SPIFFS_RESERVED:X}")
    for (a_name, a_start, a_size), (b_name, b_start, _) in zip(ordered, ordered[1:]):
        if a_start + a_size > b_start:
            raise SystemExit(f"{a_name} and {b_name} overlap")


def build_plan(args) -> tuple[list[tuple[int, Path]], str]:
    firmware = args.firmware or PORT_DIR / "build" / "cpyos_metalio_plus.bin"
    if not firmware.is_file():
        raise SystemExit(f"missing firmware {firmware}; build the target first")
    project, version = describe(firmware)
    if project != UNTESTED_PROJECT:
        raise SystemExit(f"{firmware.name}: project is {project!r}, expected {UNTESTED_PROJECT!r} "
                         "(a Read Pico or other image must never be written here)")
    if "untested" not in version and not getattr(args, "allow_versioned", False):
        raise SystemExit(f"{firmware.name}: version {version!r} is not marked untested "
                         "(pass --allow-versioned for a versioned test build)")

    layout = getattr(args, "layout", VENDOR_LAYOUT)
    if layout == VENDOR_LAYOUT:
        if getattr(args, "full_install", False):
            raise SystemExit("--full-install is only valid with --layout port")
        if not getattr(args, "no_data", False) and any(
                getattr(args, name, None) for name in ("pinyin", "hyphen", "i18n", "emoji", "core")):
            raise SystemExit("--layout vendor writes only an application image; data packs are "
                             "not written because the stock spiffs partitions are vendor-owned")
        slots = parse_partitions(VENDOR_PARTITIONS_CSV)
        if "ota_0" not in slots or "ota_1" not in slots:
            raise SystemExit(f"{VENDOR_PARTITIONS_CSV}: missing ota_0/ota_1")
        slot = getattr(args, "slot", "both")
        names = ["ota_0", "ota_1"] if slot == "both" else [slot]
        segments: list[tuple[int, Path]] = []
        for name in names:
            offset, size = slots[name]
            if firmware.stat().st_size > size:
                raise SystemExit(f"{firmware.name}: {firmware.stat().st_size} B exceeds the "
                                 f"stock {name} slot ({size} B); the full CPYOS shell does not "
                                 "fit the stock 5 MiB layout - use the bring-up or reader image, "
                                 "or a --layout port full install")
            segments.append((offset, firmware))
        return segments, version

    if layout == PORT_OTA_LAYOUT:
        # CPYOS dual-OTA layout (same table as Read Pico). app0 and app1 are
        # written with the same image so the bootloader can select either one;
        # the data packs go into spiffs with the offsets the firmware reads.
        # Without --full-install this is the app-only update path (the same
        # regions an OTA writes): both slots, never the table or the packs.
        full = getattr(args, "full_install", False)
        parts = parse_partitions(PORT_OTA_PARTITIONS_CSV)
        for name in ("app0", "app1", "spiffs"):
            if name not in parts:
                raise SystemExit(f"{PORT_OTA_PARTITIONS_CSV.name} is missing {name}")
        app0_off, app0_size = parts["app0"]
        app1_off, app1_size = parts["app1"]
        limit = min(app0_size, app1_size)
        if firmware.stat().st_size > limit:
            raise SystemExit(f"{firmware.name}: {firmware.stat().st_size} B exceeds the "
                             f"{limit} B OTA slot; trim the image before a full install")

        table = None
        if full:
            table = getattr(args, "partition_table", None) or (
                PORT_DIR / "build" / "partition_table" / "partition-table.bin")
            if not table.is_file():
                raise SystemExit(f"missing partition table {table}; build with `idf.py build` first")
        table_rows = parse_partition_rows(PORT_OTA_PARTITIONS_CSV)
        if full:
            actual_rows = read_partition_table_binary(table.read_bytes())
            if not tables_match(table_rows, actual_rows):
                raise SystemExit(
                    f"{table}: does not match {PORT_OTA_PARTITIONS_CSV.name}\n"
                    f"  csv:    {describe_table(table_rows)}\n"
                    f"  binary: {describe_table(actual_rows)}\n"
                    "Rebuild the partition table (`idf.py --preview build`) before a full install.")

        segments: list[tuple[int, Path]] = []
        if full:
            segments.append((PARTITION_TABLE_OFFSET, table))
        segments += [(app0_off, firmware), (app1_off, firmware)]
        if full and not getattr(args, "no_data", False):
            spiffs_base, spiffs_size = parts["spiffs"]
            pinyin = getattr(args, "pinyin", None) or REPO / "build" / "pinyin.dat"
            web = getattr(args, "web", None) or REPO / "build" / "web_pack.dat"
            hyphen = getattr(args, "hyphen", None) or REPO / "build" / "hyphen.dat"
            i18n = getattr(args, "i18n", None) or PORT_DIR / "build" / "i18n_pack.dat"
            emoji = getattr(args, "emoji", None) or REPO / "build" / "emoji.dat"
            core = core_blob(getattr(args, "core", None))
            for label, path in (("pinyin", pinyin), ("web", web), ("hyphen", hyphen),
                                ("i18n", i18n), ("emoji", emoji), ("core", core)):
                if not path.is_file():
                    raise SystemExit(f"missing {label} pack {path} (use --no-data to skip packs)")
            offsets = {
                "pinyin": spiffs_base,
                "web": spiffs_base + read_constant(REPO / "src" / "network" / "WebPackFormat.h",
                                                   "WEB_PACK_PARTITION_OFFSET"),
                "hyphen": spiffs_base + read_constant(
                    REPO / "lib" / "Epub" / "Epub" / "hyphenation" / "HyphenationStore.h",
                    "HYPN_PARTITION_OFFSET"),
                "i18n": spiffs_base + read_constant(REPO / "lib" / "I18n" / "I18nPack.h",
                                                    "I18N_PACK_PARTITION_OFFSET"),
                "emoji": spiffs_base + read_constant(REPO / "src" / "EmojiPackFormat.h",
                                                     "kPartitionOffset"),
                "core": spiffs_base + read_constant(REPO / "src" / "SystemCoreFont.h",
                                                    "kPartitionOffset"),
            }
            check_ranges([(label, offsets[label], path.stat().st_size)
                          for label, path in (("pinyin", pinyin), ("web", web), ("hyphen", hyphen),
                                              ("i18n", i18n), ("emoji", emoji), ("core", core))],
                         spiffs_base, spiffs_size)
            for label, path in (("pinyin", pinyin), ("web", web), ("hyphen", hyphen),
                                ("i18n", i18n), ("emoji", emoji), ("core", core)):
                segments.append((offsets[label], path))
        segments.sort(key=lambda item: item[0])
        return segments, version

    if layout != PORT_LAYOUT:
        raise SystemExit(f"unknown layout {layout!r}")
    if not getattr(args, "full_install", False):
        raise SystemExit(
            "the CPYOS single-slot layout does NOT match a stock Metalio E-INK4 Plus.\n"
            "For a first hardware test use --layout vendor with the bring-up/reader image.\n"
            "To replace the stock layout, pass --layout port --full-install and a --backup; "
            "that writes the partition table at 0x8000 and both app/data regions.")

    parts = parse_partitions(PORT_PARTITIONS_CSV)
    if "ota_0" not in parts or "spiffs" not in parts:
        raise SystemExit("partitions.csv is missing ota_0/spiffs")
    app_off, app_size = parts["ota_0"]
    spiffs_base, spiffs_size = parts["spiffs"]
    if firmware.stat().st_size > app_size:
        raise SystemExit(f"{firmware.name}: {firmware.stat().st_size} B exceeds the ota_0 slot")

    table = getattr(args, "partition_table", None) or (
        PORT_DIR / "build" / "partition_table" / "partition-table.bin")
    if not table.is_file():
        raise SystemExit(f"missing partition table {table}; build with `idf.py build` first")
    if table.stat().st_size > (0x9000 - PARTITION_TABLE_OFFSET):
        raise SystemExit(f"{table}: {table.stat().st_size} B does not fit the 0x8000 table area")
    # Never trust a stale build artifact: the table about to be written must
    # describe exactly the layout used to compute the app and pack offsets.
    table_rows = parse_partition_rows(PORT_PARTITIONS_CSV)
    actual_rows = read_partition_table_binary(table.read_bytes())
    if not tables_match(table_rows, actual_rows):
        raise SystemExit(
            f"{table}: does not match partitions.csv\n"
            f"  csv:    {describe_table(table_rows)}\n"
            f"  binary: {describe_table(actual_rows)}\n"
            "Rebuild the partition table (`idf.py --preview build`) before a full install.")

    segments: list[tuple[int, Path]] = [(PARTITION_TABLE_OFFSET, table), (app_off, firmware)]
    if not getattr(args, "no_data", False):
        pinyin = getattr(args, "pinyin", None) or REPO / "build" / "pinyin.dat"
        hyphen = getattr(args, "hyphen", None) or REPO / "build" / "hyphen.dat"
        i18n = getattr(args, "i18n", None) or PORT_DIR / "build" / "i18n_pack.dat"
        # generate_emoji_pack.py writes into the repository-level build/ dir.
        emoji = getattr(args, "emoji", None) or REPO / "build" / "emoji.dat"
        core = core_blob(getattr(args, "core", None))
        for label, path in (("pinyin", pinyin), ("hyphen", hyphen), ("i18n", i18n), ("emoji", emoji),
                            ("core", core)):
            if not path.is_file():
                raise SystemExit(f"missing {label} pack {path} (use --no-data to skip packs)")
        # PinyinDb reads the spiffs partition from offset 0; the hyphenation
        # container offset comes from the loader header (0x90000).
        pinyin_off = spiffs_base
        hyphen_off = spiffs_base + read_constant(
            REPO / "lib" / "Epub" / "Epub" / "hyphenation" / "HyphenationStore.h",
            "HYPN_PARTITION_OFFSET")
        i18n_off = spiffs_base + read_constant(REPO / "lib" / "I18n" / "I18nPack.h",
                                               "I18N_PACK_PARTITION_OFFSET")
        emoji_off = spiffs_base + read_constant(REPO / "src" / "EmojiPackFormat.h",
                                                "kPartitionOffset")
        core_off = spiffs_base + read_constant(REPO / "src" / "SystemCoreFont.h",
                                               "kPartitionOffset")
        check_ranges([("pinyin", pinyin_off, pinyin.stat().st_size),
                      ("hyphen", hyphen_off, hyphen.stat().st_size),
                      ("i18n", i18n_off, i18n.stat().st_size),
                      ("emoji", emoji_off, emoji.stat().st_size),
                      ("core", core_off, core.stat().st_size)], spiffs_base, spiffs_size)
        segments += [(pinyin_off, pinyin), (hyphen_off, hyphen), (i18n_off, i18n), (emoji_off, emoji),
                     (core_off, core)]
    segments.sort(key=lambda item: item[0])
    return segments, version


def detect_port() -> str:
    ports = sorted(Path("/dev").glob("cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem* found; pass --port")
    return str(ports[0])


# Connection flags shared by every esptool call. Normally empty; when the
# running app owns the USB-Serial/JTAG console the high-baud stub connect is
# flaky, and we fall back to a no-stub low-baud sync that leaves the chip in
# download mode, after which "no-reset" is required so it stays there.
CONNECT_ARGS: list[str] = []


def esptool(args: list[str]) -> subprocess.CompletedProcess:
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s31", *CONNECT_ARGS, *args]
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, check=False, text=True, capture_output=True)


def establish_link(port: str, baud: str) -> None:
    """Get the ROM loader talking without requiring a USB replug.

    The board has no EN reset line and the app owns the USB-Serial/JTAG
    peripheral. A plain high-baud stub connect usually works, but when it does
    not, a 115200 no-stub sync still does; it leaves the chip in download mode,
    so every later command must pass --before no-reset.
    """
    global CONNECT_ARGS
    CONNECT_ARGS = []
    probe = esptool(["--port", port, "--baud", baud, "--connect-attempts", "3", "chip_id"])
    if probe.returncode == 0 and "ESP32-S31" in (probe.stdout + probe.stderr):
        return
    CONNECT_ARGS = ["--before", "no-reset"]
    wake = esptool(["--port", port, "--baud", "115200", "--no-stub", "--after", "no-reset",
                    "--connect-attempts", "5", "chip_id"])
    if wake.returncode != 0 or "ESP32-S31" not in (wake.stdout + wake.stderr):
        print(wake.stdout + wake.stderr, file=sys.stderr)
        raise SystemExit("could not reach the ROM loader; power-cycle the board and retry")


def check_efuses(port: str, skip: bool = False) -> None:
    """Refuse a unit whose eFuses would reject an unsigned, unencrypted app."""
    if skip:
        print("warning: eFuse check skipped; a secure-boot/flash-encryption unit "
              "would reject this image and both stock slots may be overwritten")
        return
    cmd = [sys.executable, "-m", "espefuse", "--chip", "esp32s31", "--port", port, "summary"]
    print("+ " + " ".join(cmd))
    result = subprocess.run(cmd, check=False, text=True, capture_output=True)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        print(text, file=sys.stderr)
        raise SystemExit(
            "preflight: could not read eFuses. Confirm the unit is not secure-boot/"
            "flash-encrypted with `espefuse summary`, then pass --skip-efuse-check.")

    secure = re.search(r"SECURE_BOOT_EN\b[^\n]*?=\s*(\S+)", text)
    crypt = re.search(r"FLASH_CRYPT_CNT\b[^\n]*?=\s*(\d+)", text)
    secure_on = secure is not None and secure.group(1).strip().lower() in ("true", "1")
    crypt_on = crypt is not None and (int(crypt.group(1)) % 2) == 1
    if secure_on or crypt_on:
        raise SystemExit(
            "preflight: eFuse state rejects an unsigned/unencrypted image "
            f"(SECURE_BOOT_EN={secure.group(1) if secure else '?'}, "
            f"FLASH_CRYPT_CNT={crypt.group(1) if crypt else '?'}). "
            "Refusing to overwrite the stock app slots; restore the vendor signed "
            "image path or build a correctly signed/encrypted image.")
    print("efuses: secure boot off, flash encryption off")


def preflight(port: str, baud: str, layout: str, force_layout: bool = False,
              skip_efuse_check: bool = False) -> list[dict]:
    """Verify the chip, flash size and the real partition table before writing."""
    chip = esptool(["--port", port, "--baud", baud, "chip_id"])
    chip_text = chip.stdout + chip.stderr
    if chip.returncode != 0 or "ESP32-S31" not in chip_text:
        print(chip_text, file=sys.stderr)
        raise SystemExit("preflight: the connected target is not an ESP32-S31 "
                         "(hold BOOT and tap POWER to enter download mode)")

    flash = esptool(["--port", port, "--baud", baud, "flash_id"])
    flash_text = flash.stdout + flash.stderr
    if flash.returncode != 0 or "16MB" not in flash_text:
        print(flash_text, file=sys.stderr)
        raise SystemExit("preflight: the target is not a 16 MiB flash device")

    check_efuses(port, skip=skip_efuse_check)

    with tempfile.TemporaryDirectory() as tmp:
        table_path = Path(tmp) / "partition-table.bin"
        read = esptool(["--port", port, "--baud", baud, "read_flash",
                        f"0x{PARTITION_TABLE_OFFSET:X}", f"0x{PARTITION_TABLE_READ:X}",
                        str(table_path)])
        if read.returncode != 0 or not table_path.is_file():
            print(read.stdout + read.stderr, file=sys.stderr)
            raise SystemExit("preflight: could not read the partition table")
        actual = read_partition_table_binary(table_path.read_bytes())

        if layout == PORT_LAYOUT:
            # The CPYOS table has no ota_1.  If the stock otadata is initialised
            # and selects ota_1, a full install would silently boot the middle of
            # the new app image.  Refuse instead of hand-writing otadata.
            ota_path = Path(tmp) / "otadata.bin"
            ota = esptool(["--port", port, "--baud", baud, "read_flash",
                           "0xD000", "0x2000", str(ota_path)])
            if ota.returncode != 0 or not ota_path.is_file():
                print(ota.stdout + ota.stderr, file=sys.stderr)
                raise SystemExit("preflight: could not read otadata")
            ota_data = ota_path.read_bytes()
            if any(byte != 0xFF for byte in ota_data):
                raise SystemExit(
                    "preflight: otadata is initialised (the stock firmware has booted an OTA "
                    "slot). A CPYOS full install has no ota_1, so the bootloader could select "
                    "the middle of the new app image.\n"
                    "Refusing to write otadata by hand. Boot the stock firmware once with ota_0 "
                    "selected, or keep the stock layout and use --layout vendor.")

    vendor = parse_partition_rows(VENDOR_PARTITIONS_CSV)
    port_rows = parse_partition_rows(PORT_PARTITIONS_CSV)
    ota_rows = parse_partition_rows(PORT_OTA_PARTITIONS_CSV)
    actual_vendor = tables_match(vendor, actual)
    actual_port = tables_match(port_rows, actual)
    actual_port_ota = tables_match(ota_rows, actual)
    print(f"device: ESP32-S31, 16 MiB; partition table: {describe_table(actual)}")
    if layout == VENDOR_LAYOUT:
        if not actual_vendor:
            raise SystemExit(
                "preflight: the device partition table is not the stock Metalio layout.\n"
                f"  expected: {describe_table(vendor)}\n"
                f"  actual:   {describe_table(actual)}\n"
                "Refusing to write. Restore the stock firmware/partition table first, or use "
                "--layout port --full-install after a backup if this unit was already replaced.")
    elif layout == PORT_OTA_LAYOUT:
        # Re-running on a unit that already has the dual-OTA table is expected;
        # a stock table is the one-time conversion this layout is for.
        if not actual_port_ota and not actual_vendor and not force_layout:
            raise SystemExit(
                "preflight: the device partition table matches neither the stock Metalio layout "
                "nor the CPYOS dual-OTA layout.\n"
                f"  actual: {describe_table(actual)}\n"
                "Re-check the unit, or pass --force-layout only if you are certain this is the "
                "intended full install.")
    else:
        if not actual_vendor and not actual_port and not force_layout:
            raise SystemExit(
                "preflight: the device partition table matches neither the stock Metalio layout "
                "nor the CPYOS port layout.\n"
                f"  actual: {describe_table(actual)}\n"
                "Refusing to overwrite it. Re-check the unit, or pass --force-layout only if you "
                "are certain this is the intended full install.")
    return actual


def check_reuse_backup(path: Path) -> str:
    """Validate an existing factory image and return its sha256.

    Only the size is mandatory; the digest is printed so the user can compare it
    with the one recorded for the factory dump.
    """
    if not path.is_file() or path.stat().st_size != IMAGE_SIZE:
        raise SystemExit(f"{path} is not a full {IMAGE_SIZE} B backup")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_success(text: str) -> bool:
    """esptool changed its verify wording: accept both old and new."""
    return "Hash of data verified." in text or "Verification successful" in text


def verify_app_header(port: str, baud: str, offset: int) -> None:
    """Prove the app really landed at the offset the new table points at."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "app-head.bin"
        result = esptool(["--port", port, "--baud", baud, "read_flash",
                          f"0x{offset:X}", "0x100", str(path)])
        if result.returncode != 0 or not path.is_file():
            print(result.stdout + result.stderr, file=sys.stderr)
            raise SystemExit("post-write: could not read back the app header")
        data = path.read_bytes()
    if (len(data) < 0x30 or data[0] != 0xE9 or
            struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC):
        raise SystemExit(
            f"post-write: no valid app at 0x{offset:X}. INSTALL INCOMPLETE - do not "
            "power-cycle; re-run the flash, or enter BOOT/download mode (hold BOOT "
            "while tapping POWER) and restore the 16 MiB backup.")


DATA_FILES = ("pinyin.dat", "web_pack.dat", "hyphen.dat", "i18n_pack.dat", "emoji.dat", "cjk_core.dat")

sys.path.insert(0, str(REPO / "scripts"))
import data_manifest  # noqa: E402


def core_blob(args_core, manifest: Path | None = None) -> Path:
    """The reading-core carrier to write, taken from the Plus data manifest.

    build/cjk_core.dat is only an alias for "whichever tier was generated last", and the
    Plus layout takes the wider 8,105 carrier while Read Pico's XIP layout takes 3,500.
    Reading the manifest is what stops a Read Pico build on the same machine from being
    flashed into a Plus partition as if it were the Plus core.
    """
    if args_core:
        return Path(args_core)
    table = manifest or (REPO / "build" / "data_manifest_plus.txt")
    found = data_manifest.blob(REPO, table, "core") if table.is_file() else None
    return found or (REPO / "build" / "cjk_core.dat")


def ensure_data_artifacts() -> bool:
    """Make sure every data blob (including the reading core) exists.

    generate_data_manifest.py builds emoji.dat and cjk_core.dat from the
    committed sources and writes the manifest the flasher plans from; pinyin,
    hyphen, i18n and web come from the app build (or the offline generators).
    Returns False when a required blob is still missing, so a flash never
    silently leaves the device without the core font.
    """
    build = REPO / "build"
    if all((build / name).exists() for name in DATA_FILES):
        return True
    manifest = REPO / "scripts" / "generate_data_manifest.py"
    if manifest.exists():
        subprocess.run([sys.executable, str(manifest),
                        "--partitions", "ports/metalio_plus/partitions_plus_ota.csv",
                        "--out", "build/data_manifest_plus.txt"], cwd=str(REPO), check=False)
    missing = [name for name in DATA_FILES if not (build / name).exists()]
    if missing:
        print("error: missing data blobs: " + ", ".join(missing), file=sys.stderr)
        print("build the app first, or run:", file=sys.stderr)
        print("  python3 scripts/generate_data_manifest.py \\", file=sys.stderr)
        print("      --partitions ports/metalio_plus/partitions_plus_ota.csv \\", file=sys.stderr)
        print("      --out build/data_manifest_plus.txt", file=sys.stderr)
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--layout", choices=(VENDOR_LAYOUT, PORT_LAYOUT, PORT_OTA_LAYOUT),
                        default=VENDOR_LAYOUT)
    parser.add_argument("--slot", choices=("ota_0", "ota_1", "both"), default="both",
                        help="vendor layout only; both is the safe default for a first test")
    parser.add_argument("--full-install", action="store_true",
                        help="port layout only: also write the partition table at 0x8000")
    parser.add_argument("--partition-table", type=Path,
                        help="port layout: partition-table.bin (default build/partition_table/...)")
    parser.add_argument("--pinyin", type=Path)
    parser.add_argument("--hyphen", type=Path)
    parser.add_argument("--i18n", type=Path)
    parser.add_argument("--emoji", type=Path)
    parser.add_argument("--core", type=Path, help="compressed reading core (cjk_core.dat)")
    parser.add_argument("--web", type=Path, help="web asset pack (port-ota layout)")
    parser.add_argument("--allow-versioned", action="store_true",
                        help="accept a versioned (published) test build; the project check still applies")
    parser.add_argument("--no-data", action="store_true")
    parser.add_argument("--baud", default="921600")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force-layout", action="store_true",
                        help="port layout: allow overwriting an unrecognised partition table")
    parser.add_argument("--skip-efuse-check", action="store_true",
                        help="skip the secure-boot/flash-encryption eFuse check (dangerous)")
    parser.add_argument("--backup", type=Path, help="read the full 16 MiB flash first")
    parser.add_argument("--skip-backup", action="store_true",
                        help="explicit opt-out for a unit that already has a verified 16 MiB "
                             "backup (development iteration); never use it for a first install")
    parser.add_argument("--reuse-backup", type=Path, metavar="PATH",
                        help="use an existing 16 MiB factory backup instead of copying flash again "
                             "(it is still checked for size and hashed)")
    args = parser.parse_args()

    if args.layout != VENDOR_LAYOUT and not args.no_data and not ensure_data_artifacts():
        return 1
    if args.layout == VENDOR_LAYOUT and not args.no_data:
        if not any((args.pinyin, args.hyphen, args.i18n, args.emoji, args.core)):
            print("note: --layout vendor writes only the app slots; data packs are skipped")
            args.no_data = True
        else:
            raise SystemExit("--layout vendor does not write data packs; use the port layout "
                             "full install for the CPYOS spiffs packs")

    segments, version = build_plan(args)
    print(f"layout={args.layout}; firmware {version}; {len(segments)} segment(s):")
    for offset, path in segments:
        print(f"  0x{offset:06X}  {path}  ({path.stat().st_size:,} B)")
    if args.dry_run:
        return 0
    if args.skip_backup and (args.backup or args.reuse_backup):
        raise SystemExit("--skip-backup cannot be combined with --backup/--reuse-backup")
    if not args.skip_backup and not args.backup and not args.reuse_backup:
        raise SystemExit("a full 16 MiB --backup (or --reuse-backup PATH) is mandatory before "
                         "writing any slot (the stock firmware is not in this repository)")
    if args.reuse_backup and not args.skip_backup:
        digest = check_reuse_backup(args.reuse_backup)
        print(f"reusing backup {args.reuse_backup} ({IMAGE_SIZE} B) sha256 {digest}")

    port = args.port or detect_port()
    establish_link(port, args.baud)
    preflight(port, args.baud, args.layout, force_layout=args.force_layout,
              skip_efuse_check=args.skip_efuse_check)

    if args.backup and not args.skip_backup:
        if args.backup.exists():
            raise SystemExit(f"{args.backup} exists; refusing to overwrite a backup")
        result = esptool(["--port", port, "--baud", args.baud, "read_flash", "0",
                          f"0x{IMAGE_SIZE:X}", str(args.backup)])
        if result.returncode != 0 or not args.backup.is_file():
            print(result.stdout + result.stderr, file=sys.stderr)
            raise SystemExit("backup failed")
        if args.backup.stat().st_size != IMAGE_SIZE:
            raise SystemExit(f"backup {args.backup} is {args.backup.stat().st_size} B, "
                             f"expected {IMAGE_SIZE} B; refusing to write from a short backup")
        digest = hashlib.sha256(args.backup.read_bytes()).hexdigest()
        print(f"backup {args.backup} ({args.backup.stat().st_size} B) sha256 {digest}")

    # Write the app first and the partition table last. A full install rewrites
    # the table; committing it before the 9 MB app would leave an unbootable
    # table if the USB link drops mid-write. (An interrupted full install is
    # still not resumable from the device; use BOOT/download mode and the backup.)
    write_segments = sorted(segments,
                            key=lambda item: (item[0] == PARTITION_TABLE_OFFSET, item[0]))
    write_args = ["--port", port, "--baud", args.baud, "write_flash"]
    for offset, path in write_segments:
        write_args += [f"0x{offset:X}", str(path)]
    result = esptool(write_args)
    print(result.stdout + result.stderr)
    if result.returncode != 0:
        raise SystemExit("write_flash failed")

    verified = 0
    for offset, path in segments:
        check = esptool(["--port", port, "--baud", args.baud, "verify_flash",
                         f"0x{offset:X}", str(path)])
        if check.returncode == 0 and verify_success(check.stdout + check.stderr):
            verified += 1
        else:
            print(check.stdout + check.stderr, file=sys.stderr)
    if verified != len(segments):
        raise SystemExit(f"verify_flash passed {verified}/{len(segments)} segments")
    firmware = (args.firmware or PORT_DIR / "build" / "cpyos_metalio_plus.bin").resolve()
    for offset, path in segments:
        if path.resolve() == firmware:
            verify_app_header(port, args.baud, offset)
    if args.layout == VENDOR_LAYOUT:
        print("verified the application slot(s); the stock partition table, NVS, model, "
              "resources and font_data were not touched")
    else:
        print("verified the partition table, app and data packs")
    print("power-cycle to boot the new image")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
