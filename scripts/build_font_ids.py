#!/usr/bin/env python3
"""Generate `src/fontIds.h`: every renderer font id the firmware uses.

One generator, one file. Before this script the ids came from three places
(`lib/EpdFont/scripts/build-font-ids.sh`, `src/SdCardFontSystem.h` for the
`CJK_UI_*` roles, `src/components/UiHighDpiProfile.h` for the legacy 12pt
reader slot), which is how a role could be renumbered without anyone noticing.

Two kinds of id, deliberately:

* **Header-derived.** `sum(SHA-256(header)) mod 2^32`, interpreted as a signed
  32-bit value. Editing a built-in font header therefore re-keys every section
  laid out with that face. The CJK bitmap headers are *not* inputs: the reader
  face on every shipped target is the vector pack (see `CJK_READER_SIZE_FONT_ID`
  below), so folding 4.8 MB of bitmap headers into `NOTOSANS_*` would churn ids
  the pack already covers.
* **Pack-derived.** The reader size ids carry `CJK_PACK_SIGNATURE`, so a pack
  with different metrics re-keys the section cache and pagination rebuilds
  exactly once - no manual `SECTION_FILE_VERSION` bump for a font edit.

Usage:
    python3 scripts/build_font_ids.py            # write src/fontIds.h
    python3 scripts/build_font_ids.py --check    # exit 1 when the header is stale
    python3 scripts/build_font_ids.py --stdout   # print instead of writing
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILTIN_DIR = ROOT / "lib" / "EpdFont" / "builtinFonts"
PACK_FONT = ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Regular-8105.ttf"
OUTPUT = ROOT / "src" / "fontIds.h"

# Reader point sizes the pack can bind (`ReaderFontSizes.h`). The low byte of
# CJK_READER_SIZE_FONT_ID holds the point size, so the range must stay < 256.
READER_MIN_PT = 10
READER_MAX_PT = 34

# Header-derived families: id name -> the style headers hashed into it.
HEADER_FAMILIES: dict[str, tuple[str, ...]] = {
    "NOTOSANS_12_FONT_ID": tuple(f"notosans_12_{s}.h" for s in ("regular", "bold", "bolditalic", "italic")),
    "NOTOSANS_14_FONT_ID": tuple(f"notosans_14_{s}.h" for s in ("regular", "bold", "bolditalic", "italic")),
    "NOTOSANS_16_FONT_ID": tuple(f"notosans_16_{s}.h" for s in ("regular", "bold", "bolditalic", "italic")),
    "NOTOSANS_18_FONT_ID": tuple(f"notosans_18_{s}.h" for s in ("regular", "bold", "bolditalic", "italic")),
    "UI_10_FONT_ID": ("ubuntu_10_regular.h", "ubuntu_10_bold.h"),
    "UI_12_FONT_ID": ("ubuntu_12_regular.h", "ubuntu_12_bold.h"),
    "SMALL_FONT_ID": ("notosans_8_regular.h",),
    "CHINESE_CHESS_FONT_ID": ("chinese_chess_16.h",),
    "CONTROL_18_FONT_ID": ("notosans_18_regular.h", "notosans_18_bold.h"),
    "NOTOSERIF_TITLE_FONT_ID": ("notoserif_title_23.h",),
}

# Retired faces whose ids must keep working: stored `fontFamily` values and the
# emergency fallback chain still name them, and the 16 NotoSerif reader headers
# were deleted (a serif reading face belongs in the SD/FEXT channel). Frozen at
# the values the header hash produced while the headers existed.
FROZEN_IDS: dict[str, int] = {
    "NOTOSERIF_12_FONT_ID": 778531645,
    "NOTOSERIF_14_FONT_ID": -928381217,
    "NOTOSERIF_16_FONT_ID": 17214534,
    "NOTOSERIF_18_FONT_ID": 840051567,
}

# Role ids that are not derived from content: the UI slot -> point-size aliases
# the pack registers, the reader-only 18pt alias, and the legacy high-DPI 12pt
# reader slot that sections were laid out with before the pack took over.
#
# `CJK_UI_*` deliberately keeps the `0x43 4A 4B <pt>` shape its consumers and
# old caches already use, so moving them here changes no value.
CJK_UI_IDS: dict[str, int] = {
    "CJK_UI_8_FONT_ID": 0x434A4B08,
    "CJK_UI_10_FONT_ID": 0x434A4B0A,
    "CJK_UI_12_FONT_ID": 0x434A4B0C,
    "CJK_UI_14_FONT_ID": 0x434A4B0E,
    "CJK_UI_16_FONT_ID": 0x434A4B10,
    # Reader-only 18pt alias: NOTOSANS_18 is also UI display text, so it must
    # stay free to follow the UI scale.
    "CJK_UI_18_FONT_ID": 0x43554A31,
    # `UiHighDpiProfile::reader12FontId`: the offline reader face sections were
    # laid out with before the pack existed. Kept as a value so old cached
    # sections and `UiHighDpiProfile` keep naming the same face.
    "UI_HIGH_DPI_READER_12_FONT_ID": 0x4738000C,
}


def header_id(paths: list[Path]) -> int:
    """Sum of the headers' SHA-256 digests, mod 2^32, as a signed 32-bit value."""
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(f"font id input is missing: {path}")
    digest_sum = sum(int.from_bytes(hashlib.sha256(path.read_bytes()).digest(), "big") for path in paths)
    return digest_sum % (2**32) - 2**31


def pack_signature(font_path: Path = PACK_FONT) -> int:
    """Low 16 bits of the shipped pack's CRC-32.

    Covers the authoring TTF rather than the deflate carrier: the carrier is a
    derived artifact, so its bytes may change with the compressor while the
    pack's metrics do not. `test_system_font_pack.py` separately asserts the
    carrier inflates to exactly this file.
    """
    if not font_path.is_file():
        raise FileNotFoundError(f"system font pack is missing: {font_path}")
    return zlib.crc32(font_path.read_bytes()) & 0xFFFF


def font_ids(builtin_dir: Path = BUILTIN_DIR, pack_font: Path = PACK_FONT) -> dict[str, int]:
    """Every generated id, in emission order."""
    ids: dict[str, int] = {}
    for name, filenames in HEADER_FAMILIES.items():
        ids[name] = header_id([builtin_dir / filename for filename in filenames])
    ids.update(FROZEN_IDS)
    ids.update(CJK_UI_IDS)
    ids["CJK_PACK_SIGNATURE"] = pack_signature(pack_font)
    return ids


def render_header(ids: dict[str, int]) -> str:
    signature = ids["CJK_PACK_SIGNATURE"]
    reader_ids = range(READER_MIN_PT, READER_MAX_PT + 1)
    lines = [
        "// The contents of this file are generated by ./scripts/build_font_ids.py",
        "// Do not edit it: run `python3 scripts/build_font_ids.py` (or --check in CI).",
        "#pragma once",
        "",
        "// ---- Header-derived: sum(SHA-256(header)) mod 2^32, signed. ----",
        "// Editing a built-in font header therefore invalidates every section laid",
        "// out with that face.",
    ]
    lines += [f"#define {name} ({ids[name]})" for name in HEADER_FAMILIES]
    lines += [
        "",
        "// ---- Retired NotoSerif reader faces. ----",
        "// The face is gone (serif lives in the SD/FEXT channel) but the ids stay,",
        "// because stored `fontFamily` values and the emergency fallback chain name",
        "// them. Frozen at the values the deleted headers used to hash to.",
    ]
    lines += [f"#define {name} ({ids[name]})" for name in FROZEN_IDS]
    lines += [
        "",
        "// ---- Role aliases the system font pack registers. ----",
        "// One id per UI point size, plus the reader-only 18pt alias (NOTOSANS_18 is",
        "// UI display text, so the reader must not share it) and the legacy",
        "// high-DPI 12pt reader slot.",
    ]
    lines += [f"#define {name} (0x{ids[name]:08X})" for name in CJK_UI_IDS]
    lines += [
        "",
        "// ---- Pack-derived reader sizes. ----",
        "// CJK_PACK_SIGNATURE is crc32(assets/fonts/CPYOSHanSansSC-Regular-8105.ttf)",
        f"// & 0xFFFF (0x{signature:04X}). The point size lives in the low byte, so a",
        "// pack with different metrics re-keys the section cache and pagination",
        "// rebuilds exactly once - no manual SECTION_FILE_VERSION bump.",
        f"#define CJK_PACK_SIGNATURE (0x{signature:04X})",
        "#define CJK_READER_SIZE_FONT_ID(pt) (0x43000000 | (CJK_PACK_SIGNATURE << 8) | (pt))",
        "",
        "// Font ID 0 is reserved as the \"not found\" sentinel.",
        "// Guard against any hash accidentally producing 0.",
    ]
    lines += [f'static_assert({name} != 0, "Font ID collision with sentinel");' for name in ids if name in HEADER_FAMILIES or name in FROZEN_IDS or name in CJK_UI_IDS]
    lines += [
        'static_assert(CJK_PACK_SIGNATURE != 0, "Font ID collision with sentinel");',
        f'static_assert(CJK_READER_SIZE_FONT_ID({READER_MIN_PT}) != 0, "Font ID collision with sentinel");',
        f'static_assert(CJK_READER_SIZE_FONT_ID({READER_MAX_PT}) != 0, "Font ID collision with sentinel");',
        "",
        "// Two ids that collide would silently draw one role in another's face.",
        "// The whole table is checked, including the pack-derived reader sizes.",
        "namespace font_ids_detail {",
        "constexpr bool idsUnique() {",
        "  constexpr int kIds[] = {",
    ]
    lines += [f"      {name}," for name in ids]
    lines += [f"      CJK_READER_SIZE_FONT_ID({pt})," for pt in reader_ids]
    lines += [
        "  };",
        "  constexpr int kCount = static_cast<int>(sizeof(kIds) / sizeof(kIds[0]));",
        "  for (int i = 0; i < kCount; ++i) {",
        "    for (int j = i + 1; j < kCount; ++j) {",
        "      if (kIds[i] == kIds[j]) return false;",
        "    }",
        "  }",
        "  return true;",
        "}",
        "}  // namespace font_ids_detail",
        'static_assert(font_ids_detail::idsUnique(), "Font ID collision between generated ids");',
    ]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail when src/fontIds.h is not current")
    parser.add_argument("--stdout", action="store_true", help="print the header instead of writing it")
    args = parser.parse_args(argv)

    try:
        text = render_header(font_ids())
    except (FileNotFoundError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.stdout:
        sys.stdout.write(text)
        return 0
    if args.check:
        committed = OUTPUT.read_text() if OUTPUT.is_file() else ""
        if committed != text:
            print(f"error: {OUTPUT.relative_to(ROOT)} is stale; run scripts/build_font_ids.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is current")
        return 0
    OUTPUT.write_text(text)
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
