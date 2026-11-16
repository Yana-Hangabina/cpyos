#!/usr/bin/env python3
"""Independent checks for a packaged Metalio E-INK4 Plus UNTESTED bundle.

The packager writes the bundle; this verifier re-reads it from scratch and fails
on anything a release consumer would not accept:

  * all four images exist, carry an `esp_app_desc` for `cpyos_metalio_plus`
    with an `untested` version, and fit the 14.375 MiB app slot;
  * pinyin is `PYDB` (and cannot reach the reserved spiffs gap), the
    hyphenation container is `HYPN`, the language pack is `JILP`, and the emoji
    pack is `EMJ1` with a matching CRC;
  * `SHA256SUMS` covers every payload member and every digest matches;
  * the UNTESTED notice and the project/third-party licences are present.

    python3 scripts/verify_metalio_bundle.py --bundle ../../../../artifacts/cpyos-metalio-plus-0.0.0-untested.zip
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zipfile
import zlib
from pathlib import Path

APP_SLOT = 0xE60000
DESC_MAGIC = 0xABCD5432
IMAGES = ("bsp", "bringup", "reader", "shell")
LEGAL = (
    "legal/LICENSE",
    "legal/NOTICE-JJWXC.md",
    "legal/THIRD_PARTY_NOTICES.md",
    "legal/MIT.txt",
    "legal/Apache-2.0.txt",
    "legal/OFL-1.1.txt",
    "legal/AGPL-3.0.txt",
    "legal/Metalio-E-INK4-Plus-MIT.txt",
    "legal/Crosspoint-Simulator-MIT.txt",
)
META = {"SHA256SUMS", "SBOM.txt", "UNTESTED-ON-HARDWARE.txt"}


def fail(message: str) -> None:
    print("FAIL: " + message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    args = parser.parse_args()

    if not args.bundle.is_file():
        fail(f"missing {args.bundle}")
    with zipfile.ZipFile(args.bundle) as bundle:
        names = set(bundle.namelist())
        for required in set(LEGAL) | META:
            if required not in names:
                fail(f"bundle is missing {required}")

        images = {}
        for variant in IMAGES:
            matches = [n for n in names if n.startswith(f"cpyos_metalio_plus_{variant}_")]
            if len(matches) != 1:
                fail(f"expected one {variant} image, found {matches}")
            data = bundle.read(matches[0])
            if len(data) < 0xA0 or struct.unpack_from("<I", data, 0x20)[0] != DESC_MAGIC:
                fail(f"{matches[0]}: no esp_app_desc")
            version = data[0x30:0x50].split(b"\0", 1)[0].decode("utf-8", "replace")
            project = data[0x50:0x70].split(b"\0", 1)[0].decode("utf-8", "replace")
            if project != "cpyos_metalio_plus":
                fail(f"{matches[0]}: project {project!r}")
            if "untested" not in version:
                fail(f"{matches[0]}: version {version!r} is not untested")
            if len(data) > APP_SLOT:
                fail(f"{matches[0]}: {len(data)} B exceeds the app slot")
            images[variant] = matches[0]

        i18n = [n for n in names if "i18n_pack" in n]
        emoji = [n for n in names if "emoji_pack" in n]
        pinyin = [n for n in names if "pinyin" in n]
        hyphen = [n for n in names if "hyphen" in n]
        if len(i18n) != 1 or len(emoji) != 1 or len(pinyin) != 1 or len(hyphen) != 1:
            fail(f"expected one i18n/emoji/pinyin/hyphen pack, got {i18n} {emoji} {pinyin} {hyphen}")
        language = bundle.read(i18n[0])
        if len(language) < 64 or language[:4] != b"JILP":
            fail(f"{i18n[0]}: not a JILP pack")
        pinyin_data = bundle.read(pinyin[0])
        if len(pinyin_data) < 64 or pinyin_data[:4] != b"PYDB" or len(pinyin_data) >= 0x80000:
            fail(f"{pinyin[0]}: not a usable PYDB pack (reserved gap at spiffs+0x80000)")
        hyphen_data = bundle.read(hyphen[0])
        if len(hyphen_data) < 64 or hyphen_data[:4] != b"HYPN" or len(hyphen_data) > 0x70000:
            fail(f"{hyphen[0]}: not a usable HYPN pack (must end before the i18n pack)")
        emoji_data = bundle.read(emoji[0])
        if len(emoji_data) < 32 or emoji_data[:4] != b"EMJ1":
            fail(f"{emoji[0]}: not an EMJ1 pack")
        ttf_size, crc = struct.unpack_from("<II", emoji_data, 8)
        if len(emoji_data) != 32 + ttf_size or (zlib.crc32(emoji_data[32:]) & 0xFFFFFFFF) != crc:
            fail(f"{emoji[0]}: emoji size/CRC mismatch")

        sums_text = bundle.read("SHA256SUMS").decode("utf-8")
        listed = {}
        for line in sums_text.splitlines():
            if not line.strip():
                continue
            digest, _, name = line.partition("  ")
            if not name:
                fail(f"malformed SHA256SUMS line: {line!r}")
            listed[name] = digest
        for name, digest in listed.items():
            if name not in names:
                fail(f"SHA256SUMS lists missing {name}")
            actual = hashlib.sha256(bundle.read(name)).hexdigest()
            if actual != digest:
                fail(f"{name}: digest mismatch")
        payload = names - META
        missing = payload - set(listed)
        if missing:
            fail(f"SHA256SUMS does not cover: {sorted(missing)}")

        notice = bundle.read("UNTESTED-ON-HARDWARE.txt").decode("utf-8", "replace")
        # Port-OTA layout (partitions_plus_ota.csv): spiffs at 0xCD0000. The
        # single-slot readpico-style table is a different layout and must not be
        # used to validate this bundle.
        for needle in ("UNTESTED ON HARDWARE", "0xCD0000", "0xD26000", "0xD60000", "0xDD0000",
                       "0xE30000", "0xE50000", "bringup_serial"):
            if needle not in notice:
                fail(f"UNTESTED notice lacks {needle!r}")
        sbom = bundle.read("SBOM.txt").decode("utf-8", "replace")
        for needle in ("esp_tinyusb", "tinyusb", "UNTESTED ON HARDWARE"):
            if needle not in sbom:
                fail(f"SBOM lacks {needle!r}")

    print(f"bundle OK: {args.bundle.name} ({args.bundle.stat().st_size:,} B), "
          f"{len(images)} images, {len(listed)} hashed members")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
