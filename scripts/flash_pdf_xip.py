#!/usr/bin/env python3
"""Flash / update the readpico XIP PDF layout with esptool.

First installation writes the XIP partition table, both app slots, the pdfmod
engine and the unchanged data blobs.  `--pdfmod-only` updates just the engine,
which is the normal way to ship a new MuPDF build without touching the app or
the data partition.

Examples:
  python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101
  python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101 --pdfmod-only
  python3 scripts/flash_pdf_xip.py --port ... --dry-run
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DESC_MAGIC = b"\x32\x54\xcd\xab"  # esp_app_desc_t.magic_word 0xABCD5432, little endian
DESC_PROJ_OFF = 0x30              # project_name[32], per AGENTS.md section 6
VENDOR_CORE_PROJECT = "arduino-lib-builder"


def classify_boot(text: str) -> list[str]:
    """Return every first-boot fault visible in a serial capture.

    esptool only proves the bytes landed; it cannot tell whether the image
    boots.  A missing PSRAM heap and a reset before the [BOOT] line both look
    like a successful flash from the host side, so they are named here.
    """
    faults: list[str] = []
    if re.search(r"no PSRAM (?:headroom|for)", text) or re.search(r"PSRAM free=0\b", text):
        faults.append("no PSRAM heap (PSRAM is not configured or not initialised)")
    if "Highlevel framebuffer allocation failed" in text:
        faults.append("display framebuffer could not be allocated (the panel stays black)")
    if re.search(r"sdmmc_init_ocr: send_op_cond \(1\) returned 0x107", text):
        faults.append("SDMMC timed out (0x107)")
    if re.search(r"PSRAM free=0", text) and re.search(r"send_op_cond \(1\) returned 0x107", text):
        # Both 3V3 loads down at once *looks* like a supply problem, and that
        # reading was written here once and was wrong: on 2026-11-19 this exact
        # pair of lines followed an XIP engine crash and left the octal PSRAM in
        # a state no soft reset clears.  Report the shape and the one action that
        # is known to help; name no cause.
        faults.append("PSRAM and SDMMC both down after a crash: power-cycle the "
                      "device before judging anything "
                      "(docs/engineering/pdf-xip-psram-init-incident-20261119.md)")
    if "Dumped panic info" in text:
        faults.append("previous boot panicked")
    if not text.strip():
        faults.append("no serial output at all (device powered off or held in reset)")
    elif not re.search(r"\[BOOT\] boot \d+ms", text):
        faults.append("never reached the first frame (no [BOOT] line)")
    if len(re.findall(r"\brst:0x[0-9a-fA-F]", text)) >= 3:
        faults.append("reset loop: the ROM reset banner appeared three or more times")
    return faults


# --------------------------------------------------------------------------- #
# Which Arduino core configuration actually got compiled?
#
# AGENTS.md section 83: a device shipped showing a white screen because every frame
# starved (1.9 s per scan, `Frame failed (1024)` on all of them) while the app source
# was fine.  The cause was a core built from another tree's settings, and the documented
# detector -- esp_app_desc.project_name -- could not see it, because that descriptor
# comes from the SHARED libesp_app_format.a and reports whichever tree linked last.  The
# question is answerable directly instead: pioarduino writes the configuration it baked
# into the prebuilt libs to .../framework-arduinoespressif32-libs/<target>/sdkconfig.
#
# Only keys that platformio.ini actually declares are demanded, so the guard can never
# invent a requirement the project does not have -- and a missing/renamed entry simply
# drops out rather than producing a false green.
CORE_CONFIG_KEYS = (
    # Line feeding during a scan is cache/bandwidth bound: 86 us/line with the 64 KB
    # data cache versus 163 us/line without (AGENTS.md section 45.4).  A frame budget of
    # ~420 ms needs the tuned value, so these three are the difference between a working
    # display and a permanently blank one.
    "CONFIG_ESP32S3_DATA_CACHE_64KB",
    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL",
)

# Relative to the home directory.  This is pioarduino's "this custom configuration has
# been baked into the core" record -- NOT the neighbouring `sdkconfig`, which is Espressif's
# untouched reference config (it says SPIRAM_MALLOC_RESERVE_INTERNAL=0 even in a healthy
# build, so reading it produces a false alarm; measured 2026-11-20).
#
# What this guard can catch: a core that never saw our settings at all.  What it cannot
# catch, and what actually broke the device on 2026-11-20: the record present while the
# packaged libs underneath it had been replaced by another tree, so pioarduino skipped the
# rebuild (AGENTS.md section 6).  The only end-to-end detector for that case is on-device:
# a full 684-line frame must cost well under a second (SCANSTATS), because a core without
# the 64 KB data cache feeds lines at ~75 us instead of ~16 us and every frame starves.
CORE_SDKCONFIG_RELATIVES = (
    ".platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig.crosspoint",
)


def parse_sdkconfig(text: str) -> dict[str, str]:
    """Kconfig text -> values.  `# X is not set` and comments yield no entry."""
    got: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, _, value = line.partition("=")
            got[key.strip()] = value.strip()
    return got


def declared_core_config(ini_text: str) -> dict[str, str]:
    """What this repo asks the core for, read from platformio.ini's custom_sdkconfig."""
    wanted = {key: None for key in CORE_CONFIG_KEYS}
    for line in ini_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("CONFIG_") and "=" in stripped:
            key, _, value = stripped.partition("=")
            if key.strip() in wanted:
                wanted[key.strip()] = value.strip()
    return {k: v for k, v in wanted.items() if v is not None}


def core_config_problems(sdkconfig_text: str, required: dict[str, str]) -> list[str]:
    """Empty means the compiled core carries every setting the image was linked against."""
    got = parse_sdkconfig(sdkconfig_text)
    problems: list[str] = []
    for key, want in sorted(required.items()):
        have = got.get(key)
        if have is None:
            problems.append(f"{key} is absent, but platformio.ini declares {key}={want}")
        elif have != want:
            problems.append(f"{key}={have}, but platformio.ini declares {key}={want}")
    return problems


def compiled_core_problems() -> list[str]:
    """Compare the installed core's config against this repo's demands."""
    ini = ROOT / "platformio.ini"
    override = os.environ.get("CPYOS_CORE_SDKCONFIG")
    candidates = ([Path(override).expanduser()] if override else
                  [Path.home() / rel for rel in CORE_SDKCONFIG_RELATIVES])
    required = declared_core_config(ini.read_text() if ini.exists() else "")
    if not required:
        return ["platformio.ini declares none of the core settings this guard knows about"]
    for path in candidates:
        if not path.exists():
            continue
        return core_config_problems(path.read_text(), required)
    return [f"no compiled-core sdkconfig found (looked at {', '.join(str(c) for c in candidates)}); "
            "cannot prove custom_sdkconfig reached the core"]


def project_name(image: Path) -> str:
    data = image.read_bytes()
    at = data.find(DESC_MAGIC)
    if at < 0:
        return ""
    field = data[at + DESC_PROJ_OFF:at + DESC_PROJ_OFF + 32]
    return field.split(b"\0", 1)[0].decode("latin1")

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "readpico_pdf_jjwxc"
ARCHIVE = ROOT / "artifacts" / "pdf-xip" / "flashes"
PART_CSV = ROOT / "partitions_readpico_xip.csv"
PDFMOD = ROOT / "build" / "pdfmod.bin"


def partitions() -> dict[str, tuple[int, int]]:
    rows: dict[str, tuple[int, int]] = {}
    with PART_CSV.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith("#")):
            if len(row) >= 5:
                rows[row[0].strip()] = (int(row[3].strip(), 0), int(row[4].strip(), 0))
    for name in ("app0", "app1", "pdfmod", "spiffs"):
        if name not in rows:
            raise SystemExit(f"missing {name} in {PART_CSV}")
    return rows


def data_rows() -> list[tuple[int, Path]]:
    manifest = ROOT / "build" / "data_manifest.txt"
    if not manifest.is_file():
        raise SystemExit(f"missing {manifest}; run scripts/generate_data_manifest.py")
    out = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) == 7 and fields[0] == "data":
            out.append((int(fields[3], 0), ROOT / fields[6]))
    return out


def boot_app0() -> Path:
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR") or Path.home() / ".platformio")
    path = core / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
    if not path.is_file():
        raise SystemExit(f"missing {path}")
    return path


def esptool_python() -> str:
    """Prefer PlatformIO's interpreter: the system python often lacks esptool."""
    override = os.environ.get("ESPTOOL_PYTHON")
    candidates = [override] if override else []
    candidates += [str(Path.home() / ".platformio" / "penv" / "bin" / "python"), sys.executable]
    for candidate in candidates:
        if not candidate:
            continue
        try:
            probe = subprocess.run([candidate, "-c", "import esptool"], capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            continue
        if probe.returncode == 0:
            return candidate
    raise SystemExit("no Python interpreter with esptool; install it or set ESPTOOL_PYTHON")


def elf_tag(elf: Path) -> str:
    """The 9-hex-digit `ELF file SHA256:` a panic prints for this ELF.

    esptool stores SHA-256(linked ELF) at image offset 0xb0 (that is also what
    `pdfmod.bin`'s appTag must equal), and the panic handler prints the first
    nine digits of the same digest.  Recording it per flash is what makes a
    future crash line resolvable instead of orphaned.
    """
    return hashlib.sha256(elf.read_bytes()).hexdigest()[:9]


def archive_pair(elf: Path, image: Path, module: Path) -> Path:
    """Copy the exact app+engine+ELF that is about to be flashed.

    The 2026-11-19 XIP crash could not be symbolized because its
    `ELF file SHA256: 5dc4b1652` matched nothing on this machine -- the build
    directory had been reused and rebuilt since (see
    docs/engineering/pdf-xip-psram-init-incident-20261119.md section 3.1).  An
    image without its ELF is an unanswerable crash report, so keep the trio
    before writing anything.
    """
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "Z"
    out = ARCHIVE / stamp
    out.mkdir(parents=True, exist_ok=True)
    kept = {}
    for src in (elf, image, module):
        dst = out / src.name
        shutil.copyfile(src, dst)
        kept[src.name] = {
            "bytes": src.stat().st_size,
            "sha256": hashlib.sha256(src.read_bytes()).hexdigest(),
        }
    (out / "manifest.json").write_text(json.dumps(
        {"utc": stamp, "elfTag9": elf_tag(elf), "files": kept}, indent=2) + "\n")
    # A pointer file, not a symlink.  shutil.rmtree refuses to delete a symlink
    # to a directory, and ignore_errors=True hid that, so the first version of
    # this function died with FileExistsError on the second flash -- before
    # writing anything, but still a failure in the path that protects the device.
    pointer = ARCHIVE / "LATEST"
    pointer.write_text(f"{out.name}\n{elf_tag(elf)}\n")
    print(f"archived this pair in {out} (panic tag {elf_tag(elf)})")
    return out


def parse_partition_table(data: bytes) -> dict[str, tuple[int, int]]:
    """Decode a raw ESP-IDF partition table blob into {label: (offset, size)}.

    Entry = 32 bytes: magic AA 50, type, subtype, u32le offset, u32le size, 16-byte
    NUL-padded label.  Stops at the first non-entry so trailing blank space is not
    mistaken for partitions.
    """
    rows: dict[str, tuple[int, int]] = {}
    for start in range(0, len(data) - 31, 32):
        entry = data[start:start + 32]
        if entry[0] != 0xAA or entry[1] != 0x50:
            break
        label = entry[12:28].split(b"\0", 1)[0].decode("latin1")
        if not label:
            continue
        rows[label] = (int.from_bytes(entry[4:8], "little"), int.from_bytes(entry[8:12], "little"))
    return rows


def layout_shift_message(device: dict[str, tuple[int, int]],
                         target: dict[str, tuple[int, int]]) -> str | None:
    """Explain why writing `target` over this device would orphan the data blobs.

    The five blobs (pinyin/web/i18n/emoji/cjk_core) are read by absolute partition
    offset, and this tree's two readpico tables deliberately share `spiffs`.  That
    equivalence holds between the files, NOT against whatever table the device
    actually carries -- a device that was last flashed from another branch may sit on
    a different spiffs base entirely.  Writing a table that moves `spiffs` and then
    flashing only the app produces AGENTS.md section 29.1's failure exactly: the
    device boots, the language pack / pinyin / hyphenation are silently dead, and
    esptool reports `Hash of data verified.` for everything it did write.
    """
    here, there = device.get("spiffs"), target.get("spiffs")
    if here is None or there is None:
        return None  # cannot say; the caller reports the unknown instead
    if here == there:
        return None
    return (f"the device's spiffs partition is at 0x{here[0]:x} ({here[1]:x} B) but this "
            f"image's table puts it at 0x{there[0]:x} ({there[1]:x} B), so the firmware "
            f"about to be written would read the five data blobs at offsets this device's "
            f"table does not have. Re-run with --full, which writes the partition table, "
            f"the app and the data blobs in one operation.")


def layout_decision(device: dict[str, tuple[int, int]] | None,
                    target: dict[str, tuple[int, int]], *, full: bool) -> tuple[bool, str | None]:
    """May this image be written onto a device whose table we just read?

    Three cases, and the middle one is the bug this function exists to prevent:
    * the tables agree -- proceed, quietly;
    * they differ and `--full` is in play -- proceed, because --full rewrites the
      partition table *and* all five blobs in the same operation, which is exactly the
      remedy the message asks for. Guarding this case was the old inverted behaviour:
      it blocked the safe path and let the dangerous one through unchecked;
    * they differ without `--full` -- refuse. An app-only flash would leave the new
      firmware reading blob offsets this device's table does not have, and esptool
      would still print `Hash of data verified.` for everything it wrote.
    """
    if device is None:
        return False, ("warning: could not read the device's partition table; cannot prove the "
                       "data partition stays put under this image. Re-run with "
                       "--skip-layout-check only if you know the device already carries this "
                       "table.")
    why = layout_shift_message(device, target)
    if why is None:
        return True, (f"device spiffs matches this image's table (0x{device['spiffs'][0]:x}); "
                      "data blobs stay valid")
    if full:
        return True, ("note: this reinstall moves spiffs; the data blobs are rewritten with "
                      "it, which is the supported way to change the layout")
    return False, f"refusing to write this image: {why}"


def read_device_partitions(port: str) -> dict[str, tuple[int, int]] | None:
    """Read the live partition table (0x8000, 3 KiB).  None means "could not look"."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as handle:
        path = handle.name
    try:
        probe = subprocess.run([esptool_python(), "-m", "esptool", "--chip", "esp32s3", "--port", port,
                                # hard-reset, not no-reset: the read drops the device into the
                                # ROM bootloader, and this board has no BOOT/RESET button, so
                                # leaving it there would need a physical power cycle.
                                "--before", "default-reset", "--after", "hard-reset", "read_flash",
                                "0x8000", "0xC00", path], capture_output=True, text=True)
        if probe.returncode != 0 or not os.path.isfile(path):
            return None
        with open(path, "rb") as stream:
            return parse_partition_table(stream.read())
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def check_pairing(elf: Path, module: Path) -> None:
    """Refuse to flash an app/engine pair that does not belong to one build.

    Runtime compatibility is decided by engineAbi/hostApi/layoutKey (see
    PdfModuleApi.h), not by the ELF digest.  A *flash* is different: the two
    images are written together, so a build-id mismatch here means the operator
    picked a stale pdfmod.bin, which is exactly the mistake this check exists to
    catch before the partition is erased.
    """
    if not elf.is_file():
        raise SystemExit(f"missing {elf}; cannot verify the app/module pairing")
    digest = hashlib.sha256(elf.read_bytes()).digest()
    with module.open("rb") as stream:
        head = stream.read(100)
    if len(head) != 100:
        raise SystemExit("pdfmod.bin header is short")

    def header_int(name: str, path: Path) -> int:
        text = path.read_text(encoding="utf-8")
        m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", text)
        if not m:
            raise SystemExit(f"{name} not found in {path.name}")
        return int(m.group(1), 0)

    api = ROOT / "src" / "pdf" / "PdfModuleApi.h"
    host = ROOT / "src" / "pdf" / "PdfModuleHost.h"
    want_version = header_int("kPdfModuleVersion", api)
    want_abi = header_int("kPdfModuleAbiVersion", api)
    want_major = header_int("kPdfModuleHostApiMajor", host)
    want_minor = header_int("kPdfModuleHostApiMinor", host)
    layout_keys = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", api.read_text(encoding="utf-8"))
    if len(layout_keys) < 2:
        raise SystemExit("kPdfModuleLayoutKey literals not found")
    want_layout = int(layout_keys[1], 16)  # flash mapping (readpico)

    version = int.from_bytes(head[4:6], "little")
    abi = int.from_bytes(head[6:8], "little")
    host_api = int.from_bytes(head[8:12], "little")
    layout = int.from_bytes(head[12:16], "little")
    build = head[68:100]
    if version != want_version or abi != want_abi:
        raise SystemExit(
            f"pdfmod.bin declares format/ABI {version}/{abi}, this firmware expects "
            f"{want_version}/{want_abi}. Rebuild the module from the current tree.")
    if (host_api >> 16) != want_major or (host_api & 0xFFFF) > want_minor:
        raise SystemExit(
            f"pdfmod.bin host API {host_api >> 16}.{host_api & 0xFFFF} is not compatible with "
            f"{want_major}.{want_minor}")
    if layout != want_layout:
        raise SystemExit(
            f"pdfmod.bin layout key 0x{layout:08X} != this firmware's 0x{want_layout:08X}; "
            "it was linked for a different memory window")
    if build != digest:
        raise SystemExit(
            "pdfmod.bin was built from a different firmware ELF (build id mismatch). "
            "Rebuild it: python3 scripts/build_pdf_module.py --firmware-elf "
            ".pio/build/readpico_pdf_jjwxc/firmware.elf  (or pass --skip-pairing-check)")


def capture_boot(port: str, seconds: float) -> str:
    """Read the console for `seconds` after a hard reset.

    An unusable port is a different fact from a silent device, so it exits with
    its own message instead of feeding an empty capture to classify_boot().
    """
    probe = subprocess.run([esptool_python(), "-m", "esptool", "--chip", "esp32s3", "--port", port,
                            "--before", "default-reset", "--after", "hard-reset", "chip-id"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        raise SystemExit(
            f"flash written, but the console check could not attach to {port}: "
            f"{probe.stderr.strip().splitlines()[-1] if probe.stderr.strip() else 'no detail'}\n"
            "Stop any serial monitor (pkill -f mon_device.py), power the device on, then re-run "
            "with --pdfmod-only --dry-run to confirm, or read the boot yourself.")
    time.sleep(0.5)
    try:
        import serial  # provided by PlatformIO's interpreter, not always by sys.executable
    except ImportError:
        raise SystemExit(f"flash written, but {esptool_python()} has no pyserial; read the console manually")
    chunks: list[str] = []
    deadline = time.time() + seconds
    try:
        stream = serial.Serial()
        stream.port = port
        stream.baudrate = 115200
        stream.timeout = 1
        stream.dtr = False
        stream.rts = False
        stream.open()
        try:
            stream.reset_input_buffer()
            while time.time() < deadline:
                chunks.append(stream.read(4096).decode("utf-8", "replace"))
        finally:
            stream.close()
    except OSError as exc:
        raise SystemExit(f"flash written, but {port} could not be opened for the boot check: {exc}")
    return "".join(chunks)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    # 921600 stalls before the stub handshake on some USB-Serial/JTAG ports;
    # 460800 completed app0+app1+pdfmod reliably and is still fast enough.
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--pdfmod-only", action="store_true", help="only erase/write the pdfmod partition")
    ap.add_argument("--data-only", action="store_true", help="only write the spiffs data blobs")
    ap.add_argument("--skip-pairing-check", action="store_true",
                   help="flash an app even if its ELF does not match the module appTag")
    ap.add_argument("--allow-vendor-core", action="store_true",
                    help="skip the compiled-core config check (and the vendor project_name check)")
    ap.add_argument("--first-install", action="store_true",
                    help="also write the XIP partition table (bootloader, partitions, boot_app0)")
    ap.add_argument("--full", action="store_true",
                    help="first install plus the data blobs (web/i18n/emoji/core)")
    ap.add_argument("--skip-layout-check", action="store_true",
                    help="do not compare the device's live partition table with this image's")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-verify-boot", action="store_true",
                    help="skip the post-flash first-boot console check")
    ap.add_argument("--boot-seconds", type=float, default=20.0)
    args = ap.parse_args()
    p = partitions()
    if not PDFMOD.is_file():
        raise SystemExit(f"missing {PDFMOD}; run scripts/build_pdf_module.py")
    if not (BUILD / "firmware.bin").is_file() or not (BUILD / "partitions.bin").is_file():
        raise SystemExit(f"missing {BUILD}/firmware.bin or partitions.bin; build -e readpico_pdf_jjwxc")

    app_size = (BUILD / "firmware.bin").stat().st_size
    for slot in ("app0", "app1"):
        if app_size > p[slot][1]:
            raise SystemExit(f"firmware {app_size} B does not fit {slot} ({p[slot][1]} B)")
    module = PDFMOD.stat().st_size
    if module > p["pdfmod"][1]:
        raise SystemExit(f"pdfmod {module} B does not fit ({p['pdfmod'][1]} B)")

    # Any flash that touches the app or the engine pairs the two; a stale module
    # against a fresh app is the crash we now refuse to ship.
    if not args.skip_pairing_check and not args.data_only:
        check_pairing(BUILD / "firmware.elf", PDFMOD)

    # A vendor-core link silently drops every custom_sdkconfig setting, PSRAM
    # tuning included (AGENTS.md section 6).  The build still says SUCCESS and
    # esptool still verifies, so this is the only host-side place that can catch
    # it.  It proves only what §6 proves: a good name does NOT prove the device
    # boots, which is why the console check below is separate.
    if not args.data_only and not args.allow_vendor_core:
        name = project_name(BUILD / "firmware.bin")
        if name == VENDOR_CORE_PROJECT:
            raise SystemExit(
                f"refusing to flash: esp_app_desc.project_name is {VENDOR_CORE_PROJECT!r}, so "
                "custom_sdkconfig never reached the Arduino core (AGENTS.md section 6). "
                "Rebuild with a recompiled core, or pass --allow-vendor-core to flash anyway.")
        print(f"image project_name={name or '<none>'} "
              "(descriptor metadata only: it comes from the shared libesp_app_format.a, "
              "so it does NOT prove which core was linked -- AGENTS.md section 83)")

    # The check that actually means something: the settings baked into the prebuilt core.
    if not args.data_only and not args.allow_vendor_core:
        if problems := compiled_core_problems():
            raise SystemExit(
                "refusing to flash: the core's applied-config record does not carry this "
                "repo's custom_sdkconfig, so the display/PSRAM tuning is absent\n  "
                + "\n  ".join(problems)
                + "\nFix (AGENTS.md section 83): back up and delete "
                  "framework-arduinoespressif32-libs/esp32s3/sdkconfig.crosspoint, clear "
                  ".dummy/CMakeLists.txt/sdkconfig.default*/.pio/build/<env>, rebuild with a "
                  "private PLATFORMIO_BUILD_CACHE_DIR, then re-verify with SCANSTATS "
                  "(a full frame must cost ~640 ms, not 1.9 s).")
        print("core config check: the applied-config record carries this repo's cache/PSRAM "
              "settings (still confirm with SCANSTATS after boot: ~640 ms per full frame)")

    # Anything that touches the app or the engine gets its ELF kept first, so
    # the panic report of the run we are about to start can be resolved later.
    if not args.dry_run and not args.data_only:
        archive_pair(BUILD / "firmware.elf", BUILD / "firmware.bin", PDFMOD)

    # Moving the data partition is the one action here that fails silently, and the
    # one thing the local files cannot answer: the device may be sitting on another
    # branch's layout. Checked before anything is archived or written, and also under
    # --dry-run, because that flag exists to reveal exactly this kind of surprise.
    # It runs on every path that writes an app image, not only the paths that write a
    # partition table -- the app-only flash is what people actually run, and it is the
    # one that strands the blobs.
    writes_an_app = not (args.data_only or args.pdfmod_only)
    if writes_an_app and not args.skip_layout_check:
        device = read_device_partitions(args.port)
        allowed, message = layout_decision(device, p, full=args.full)
        if message:
            print(message, file=sys.stderr if not allowed else sys.stdout)
        if not allowed:
            return 1
    elif args.skip_layout_check and writes_an_app:
        print("layout check skipped by request: the device's table was not compared with "
              "this image's", file=sys.stderr)

    writes: list[tuple[int, Path]] = []
    if args.pdfmod_only:
        writes.append((p["pdfmod"][0], PDFMOD))
    elif args.data_only:
        writes.extend(data_rows())
    else:
        first = args.full or args.first_install
        if first:
            writes.extend([
                (0x0, BUILD / "bootloader.bin"),
                (0x8000, BUILD / "partitions.bin"),
                (0xE000, boot_app0()),
            ])
        writes.extend([
            (p["app0"][0], BUILD / "firmware.bin"),
            (p["app1"][0], BUILD / "firmware.bin"),
            (p["pdfmod"][0], PDFMOD),
        ])
        if args.full:
            writes.extend(data_rows())
    print("flash plan:")
    for address, path in writes:
        print(f"  0x{address:06x}  {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    if args.dry_run:
        return 0

    cmd = [esptool_python(), "-m", "esptool", "--chip", "esp32s3", "--port", args.port, "--baud", args.baud,
           "write_flash"]
    for address, path in writes:
        cmd += [hex(address), str(path)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    # esptool's digest only proves the bytes landed.  Read the first boot, or
    # say plainly that we could not, so nobody mistakes an unverifiable flash
    # for a working device.
    if args.no_verify_boot or args.data_only:
        print("boot check skipped; read the console yourself before using the device")
        return 0
    text = capture_boot(args.port, args.boot_seconds)
    faults = classify_boot(text)
    if faults:
        print("first boot:", "; ".join(faults), file=sys.stderr)
        print(text[-2000:], file=sys.stderr)
        return 1
    print("first boot ok:", re.search(r"\[BOOT\] boot [^\r\n]*", text).group(0))
    for line in text.splitlines():
        if "[PDFM]" in line or "engine loaded" in line:
            print("  ", line.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
