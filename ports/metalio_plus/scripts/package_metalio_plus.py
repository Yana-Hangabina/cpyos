#!/usr/bin/env python3
"""Package the Metalio E-INK4 Plus images as an explicit UNTESTED bundle.

This is bench-distribution only: it is not an OTA channel, no manifest points
at it, and it refuses any image whose esp_app_desc does not carry the
`untested` label or that does not fit the single 14.375 MiB app slot. The
bundle also carries the pinyin and hyphenation packs, so offline pinyin input
and hyphenation work without an SD card.
"""

import argparse
import hashlib
import re
import struct
import sys
import zipfile
import zlib
from pathlib import Path

APP_SLOT = 0x9A0000  # single 9.625 MiB app slot
EMOJI_SLOT = 0x20000  # 128 KiB reserved at the end of spiffs
ESP_APP_DESC_MAGIC = 0xABCD5432
VARIANTS = ("bsp", "bringup", "reader", "shell")

UNTESTED = """CPYOS on Metalio E-INK4 Plus — UNTESTED ON HARDWARE
=====================================================

NOTHING IN THIS BUNDLE HAS RUN ON A REAL DEVICE.

* Do not treat it as a release, an OTA image, or a supported product.
* Never flash these files to a Read Pico or any other ESP32 board: the Plus is
  an ESP32-S31 with a different panel interface, power topology and partition
  table. Flashing the wrong image can brick the device.
* VCOM in the vendored config is the vendor default (1200 mV), not a
  measurement of your unit. Do not raise it without measuring; a wrong VCOM can
  permanently damage the panel.
* The STOCK machine has two 5 MiB app slots (ota_0@0x80000, ota_1@0x580000),
  NOT the CPYOS single-slot table. For the first test flash the bring-up or
  reader image with `flash_metalio_plus.py --layout vendor --slot both`; the
  9.28 MB shell DOES NOT FIT a stock 5 MiB slot.
* The single 9.625 MiB app slot + 6.25 MiB spiffs table is a destructive
  `--layout port --full-install` (rewrites the table at 0x8000, requires a full
  backup). Data pack offsets pinyin 0xCD0000 / web 0xD26000 / hyphenation
  0xD60000 / language 0xDD0000 / emoji 0xE30000 / core 0xE50000 apply only to
  that layout; the stock partitions are vendor-owned and are not written. SD
  copies (/pinyin.dat, /hyphen.dat) take precedence when present.

Use the serial bring-up console (`help`) for the first boot: info / batt / rtc /
sd / imu / touch / keys / slpinfo / haptic / panel / window / screenshot /
crypto / wifi / ap / web / usbmsc / light. `scripts/bringup_serial.py` runs the read-only
checklist and saves a framebuffer BMP.

`web <ssid> [pass]` starts the HTTP server. Browser: `/` (official home),
`/files` and `/files/en` (file manager + JSZip folder upload), `/fonts` (SD font
upload/delete), `/settings` (read-only + frontlight), `/diag` (self-check) and
`/simple` (minimal page). WebDAV and the port-81 Calibre upload socket are on
the same server. `usbmsc on` detaches the app
filesystem and exposes the card to USB; the console may disappear and a power
cycle is required to return to normal operation.
"""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def describe(path: Path) -> tuple[str, str]:
    data = path.read_bytes()
    if len(data) < 0x80:
        raise SystemExit(f"{path.name}: too small for esp_app_desc")
    magic, = struct.unpack_from("<I", data, 0x20)
    if magic != ESP_APP_DESC_MAGIC:
        raise SystemExit(f"{path.name}: no esp_app_desc magic at 0x20")
    version = data[0x30:0x50].split(b"\0")[0].decode("ascii", "replace")
    project = data[0x50:0x70].split(b"\0")[0].decode("ascii", "replace")
    return project, version


def read_lock_versions(path: Path) -> list[tuple[str, str]]:
    """Extract (component, version) pairs from an IDF dependencies.lock.

    The file is YAML, but the shape we need is fixed and shallow: a two-space
    indented ``namespace/name:`` followed by four-space indented fields. Parsing
    it with regexes keeps the packager stdlib-only.
    """
    if not path.is_file():
        return []
    components: list[tuple[str, str]] = []
    current: str | None = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if re.match(r"^  [^\s:]+/[^\s:]+:$", line):
            current = line.strip()[:-1]
            continue
        if current and re.match(r"^    version:", line):
            components.append((current, line.split(":", 1)[1].strip().strip("'")))
            current = None
    return components


def build_sbom(root: Path, lockfile: Path, idf_version: str, target: str) -> str:
    licenses = {
        "espressif/esp_tinyusb": "Apache-2.0",
        "espressif/tinyusb": "MIT",
    }
    lines = [
        "CPYOS Metalio E-INK4 Plus — software bill of materials",
        "project: cpyos_metalio_plus",
        f"target: {target}",
        f"idf: {idf_version or 'unknown'}",
        "managed components:",
    ]
    components = read_lock_versions(lockfile)
    if not components:
        lines.append("  (none recorded; run a build with the component manager first)")
    for name, version in components:
        lines.append(f"  {name} {version} ({licenses.get(name, 'see THIRD_PARTY_NOTICES.md')})")
    lines += [
        "",
        "Vendored in this repository (see THIRD_PARTY_NOTICES.md for full notices):",
        "  Metalio E-INK4 Plus vendor BSP (MIT)",
        "  ArduinoJson (MIT), JPEGDEC/PNGdec/AnimatedGIF (Apache-2.0), PubSubClient (MIT)",
        "  FreeType (FTL/GPLv2), Noto Sans CJK / Noto Emoji (SIL OFL 1.1), Lucide icons (ISC)",
        "  epdiy vendor fork (LGPL-3.0-or-later)",
        "",
        "The CPYOS custom project license and the JJWXC statement are in LICENSE and",
        "NOTICE-JJWXC.md. This bundle is UNTESTED ON HARDWARE.",
    ]
    return "\n".join(lines) + "\n"


def check_aux_pack(path: Path, magic: bytes, label: str, limit: int) -> str:
    """Pinyin/hyphenation containers are read by their own loaders; the bundle
    only checks the magic and that the blob cannot run into the next slot."""
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != magic:
        raise SystemExit(f"{path.name}: not a {label} pack")
    if len(data) > limit:
        raise SystemExit(f"{path.name}: {len(data)} B does not fit before the next pack")
    return f"{label} {len(data):,} B"


def read_key_header(path: Path) -> tuple[int, int, int | None]:
    text = path.read_text(encoding="utf-8", errors="replace")
    count = re.search(r"I18N_KEY_COUNT\s*=\s*(\d+)", text)
    signature = re.search(r"I18N_KEY_SIGNATURE\s*=\s*0x([0-9A-Fa-f]+)", text)
    content = re.search(r"I18N_CONTENT_SIGNATURE\s*=\s*0x([0-9A-Fa-f]+)", text)
    if count is None or signature is None:
        raise SystemExit(f"{path}: cannot read I18N_KEY_COUNT / I18N_KEY_SIGNATURE")
    return int(count.group(1)), int(signature.group(1), 16), (
        int(content.group(1), 16) if content else None)


def check_emoji_pack(pack: Path) -> str:
    """The emoji blob is a 32-byte EMJ1 header plus the raw TTF. A stale or
    truncated blob makes the loader disable emoji for the whole session, so
    validate the header, payload length and CRC here rather than on device."""
    data = pack.read_bytes()
    if len(data) < 32 or data[:4] != b"EMJ1":
        raise SystemExit(f"{pack.name}: not an EMJ1 emoji pack")
    version, = struct.unpack_from("<B", data, 4)
    ttf_size, crc, = struct.unpack_from("<II", data, 8)
    problems = []
    if version != 1:
        problems.append(f"version {version} != 1")
    if ttf_size < 512 or ttf_size > EMOJI_SLOT - 32:
        problems.append(f"ttfSize {ttf_size} outside the 128 KiB slot")
    if len(data) != 32 + ttf_size:
        problems.append(f"file size {len(data)} != header + ttfSize ({32 + ttf_size})")
    elif zlib.crc32(data[32:]) & 0xFFFFFFFF != crc:
        problems.append("TTF CRC does not match the header")
    if problems:
        raise SystemExit(f"{pack.name}: emoji pack is invalid: " + "; ".join(problems))
    return f"TTF {ttf_size:,} B, CRC ok"


def check_core_pack(pack: Path) -> str:
    """CPFZ: the compressed 8,105-character reading core. The firmware inflates
    it once to /fonts/CPYOSCJKCore; a truncated or mis-CRC blob would silently
    fall back to the UI subset, so validate it here."""
    data = pack.read_bytes()
    if len(data) < 32 or data[:4] != b"CPFZ":
        raise SystemExit(f"{pack.name}: not a CPFZ core pack")
    version, header_size, raw_size, raw_crc, packed = struct.unpack_from("<HHIII", data, 4)
    problems = []
    if version != 1:
        problems.append(f"version {version} != 1")
    if header_size != 32:
        problems.append(f"headerSize {header_size} != 32")
    if packed != len(data) - 32:
        problems.append(f"compressedSize {packed} != file size - 32")
    if raw_size == 0 or raw_size > 4 * 1024 * 1024:
        problems.append(f"rawSize {raw_size} outside 1..4 MiB")
    if packed > 0x1A0000:
        problems.append(f"{packed} B exceeds the 1.625 MiB core area")
    try:
        inflater = zlib.decompressobj(-15)
        raw = inflater.decompress(data[32:]) + inflater.flush()
        if not inflater.eof or len(raw) != raw_size:
            problems.append(f"inflated {len(raw)} B, expected {raw_size} B")
        elif zlib.crc32(raw) & 0xFFFFFFFF != raw_crc:
            problems.append("raw CRC does not match the header")
    except zlib.error as error:
        problems.append(f"deflate failed: {error}")
    if problems:
        raise SystemExit(f"{pack.name}: core pack is invalid: " + "; ".join(problems))
    return f"raw {raw_size:,} B, CRC ok"


def check_pack(pack: Path, keys_header: Path) -> str:
    """The pack is positional; a key-count or key-order mismatch makes every
    packed language fall back to English on device. Refuse to bundle that."""
    data = pack.read_bytes()
    if len(data) < 64 or data[:4] != b"JILP":
        raise SystemExit(f"{pack.name}: not a JILP language pack")
    key_count, key_signature, content_signature = read_key_header(keys_header)
    pack_keys, = struct.unpack_from("<I", data, 8)
    pack_signature, = struct.unpack_from("<I", data, 12)
    pack_langs, = struct.unpack_from("<H", data, 16)
    total_size, = struct.unpack_from("<I", data, 32)
    pack_content, = struct.unpack_from("<I", data, 36)
    problems = []
    if pack_keys != key_count:
        problems.append(f"keyCount {pack_keys} != firmware {key_count}")
    if pack_signature != key_signature:
        problems.append(f"keySignature {pack_signature:#x} != firmware {key_signature:#x}")
    if content_signature is not None and pack_content != content_signature:
        problems.append(f"contentSignature {pack_content:#x} != firmware {content_signature:#x}")
    if total_size != len(data):
        problems.append(f"totalSize {total_size} != file size {len(data)}")
    if pack_langs == 0:
        problems.append("no packed languages")
    if problems:
        raise SystemExit(f"{pack.name}: language pack does not match the firmware: " + "; ".join(problems))
    return f"{pack_keys} keys, {pack_langs} languages"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--version", default="0.0.0-untested")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--lockfile", type=Path, default=None,
                        help="IDF dependencies.lock (default: next to build/esp-idf)")
    parser.add_argument("--idf-version", default="",
                        help="ESP-IDF version string recorded in the SBOM")
    parser.add_argument("--keys-header", type=Path, default=None,
                        help="generated I18nKeys.h to validate the pack against "
                             "(default: build/esp-idf/cpyos_ui/port_i18n/I18nKeys.h)")
    args = parser.parse_args()

    images = {}
    for variant in VARIANTS:
        matches = sorted(args.artifacts.glob(f"cpyos_metalio_plus_{variant}_*_untested.bin"))
        if not matches:
            raise SystemExit(f"missing {variant} image in {args.artifacts}")
        image = matches[-1]
        project, version = describe(image)
        if project != "cpyos_metalio_plus":
            raise SystemExit(f"{image.name}: project {project!r} is not the Plus port")
        if "untested" not in version:
            raise SystemExit(f"{image.name}: version {version!r} is not marked untested")
        if image.stat().st_size > APP_SLOT:
            raise SystemExit(f"{image.name}: {image.stat().st_size} B exceeds the app slot")
        images[variant] = (image, project, version)

    packs = sorted(args.artifacts.glob("cpyos_metalio_plus_i18n_pack_*_untested.dat"))
    if not packs:
        raise SystemExit(f"missing i18n pack in {args.artifacts}")
    pack = packs[-1]
    keys_header = args.keys_header or (Path(__file__).resolve().parents[1] /
                                       "build/esp-idf/cpyos_ui/port_i18n/I18nKeys.h")
    if not keys_header.is_file():
        raise SystemExit(f"missing generated {keys_header}; build the shell first")
    pack_summary = check_pack(pack, keys_header)

    emojis = sorted(args.artifacts.glob("cpyos_metalio_plus_emoji_pack_*_untested.dat"))
    if not emojis:
        raise SystemExit(f"missing emoji pack in {args.artifacts}")
    emoji = emojis[-1]
    emoji_summary = check_emoji_pack(emoji)

    entries = []
    for variant, (image, project, version) in images.items():
        entries.append((image.name, image))
        print(f"  {variant:8s} {image.stat().st_size:>9,d} B  {project} {version}")
    entries.append((pack.name, pack))
    print(f"  {'i18n':8s} {pack.stat().st_size:>9,d} B  ({pack_summary}; flash at 0xDD0000)")
    entries.append((emoji.name, emoji))
    print(f"  {'emoji':8s} {emoji.stat().st_size:>9,d} B  ({emoji_summary}; flash at 0xE30000)")

    pinyin = args.root / "build" / "pinyin.dat"
    hyphen = args.root / "build" / "hyphen.dat"
    for label, path in (("pinyin", pinyin), ("hyphen", hyphen)):
        if not path.is_file():
            raise SystemExit(f"missing {path}; run scripts/generate_pinyin_db.py or "
                             "scripts/generate_hyphenation_db.py first")
    pinyin_summary = check_aux_pack(pinyin, b"PYDB", "pinyin", 0x80000)
    hyphen_summary = check_aux_pack(hyphen, b"HYPN", "hyphenation", 0x70000)
    # Reuse the artifact file names when they exist so the bundle member and
    # the file a user flashes by hand are the same bytes under the same name.
    def pack_name(kind: str, fallback: str) -> str:
        found = sorted(args.artifacts.glob(f"cpyos_metalio_plus_{kind}_*_untested.dat"))
        return found[-1].name if found else fallback

    entries.append((pack_name("pinyin", "pinyin.dat"), pinyin))
    entries.append((pack_name("hyphen", "hyphen.dat"), hyphen))
    print(f"  {'pinyin':8s} {pinyin.stat().st_size:>9,d} B  ({pinyin_summary}; flash at 0xCD0000)")
    print(f"  {'hyphen':8s} {hyphen.stat().st_size:>9,d} B  ({hyphen_summary}; flash at 0xD60000)")

    core = args.root / "build" / "cjk_core.dat"
    if not core.is_file():
        raise SystemExit(f"missing {core}; run scripts/build_ui_font_subset.py --core-only")
    core_summary = check_core_pack(core)
    entries.append((pack_name("core", "cjk_core.dat"), core))
    print(f"  {'core':8s} {core.stat().st_size:>9,d} B  ({core_summary}; flash at 0xE50000)")

    legal = [args.root / "LICENSE", args.root / "THIRD_PARTY_NOTICES.md", args.root / "NOTICE-JJWXC.md"]
    legal += sorted((args.root / "LICENSES").glob("*")) if (args.root / "LICENSES").is_dir() else []
    for path in legal:
        if path.is_file():
            entries.append((f"legal/{path.name}", path))

    port = Path(__file__).resolve().parents[1]
    lockfile = args.lockfile or (port / "dependencies.lock")
    sbom = build_sbom(args.root, lockfile, args.idf_version, "esp32s31")

    sums = "".join(f"{sha256(path)}  {name}\n" for name, path in entries)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for name, path in entries:
            bundle.write(path, name)
        bundle.writestr("UNTESTED-ON-HARDWARE.txt", UNTESTED)
        bundle.writestr("SHA256SUMS", sums)
        bundle.writestr("SBOM.txt", sbom)
    print(f"wrote {args.out} ({args.out.stat().st_size:,} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
