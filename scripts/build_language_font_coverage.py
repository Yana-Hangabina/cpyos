#!/usr/bin/env python3
"""Generate src/LanguageFontCoverage.h: per-language font requirements, at build time.

Why a table and not a runtime scan
---------------------------------
The language picker has to answer, before the user commits to a language, "can this
device actually draw it?".  Scanning 36 translation catalogues at boot costs SD reads
and PSRAM for an answer that is fixed the moment the translations and the font assets
are committed -- and getting it wrong is what produced the tofu reports after a
language switch: the setting was applied first and the missing glyphs were discovered
afterwards (docs/plans/2026-11-26-readpico-unified-pdf-channel-and-fonts.md sections
4 and 6.3).

So the answer is computed here from the same inputs the shipped assets are built from,
and compiled in.  The gate it feeds is blocking: SETTINGS.language is only written
after the required font is installed and verified, so the interface always stays in a
language the user can read.

Tiers (docs/engineering/font-tiers.md)
-------------------------------------
  Tier 0   assets/fonts/CPYOSHanSansSC-UI.ttf      app0, CJK side of the UI copy
  Tier 0b  assets/fonts/CPYOSWorldScripts-UI.ttf   app0, other scripts of the UI copy
  Tier 1   assets/fonts/CPYOSHanSansSC-Core.ttf    data-partition carrier, 3,500
  Tier 2   site/fonts/CPYOSCJKCore.ttf             downloadable, 8,105
  Tier 3   site/fonts/CPYOSWorldScripts.ttf        downloadable, full world scripts
           site/fonts/CPYOSCJK.ttf                 downloadable, CJK + Ext A
           site/fonts/NotoEmoji-Full.ttf           downloadable, full emoji

Usage (no fontTools needed, so CI can run it):
  python3 scripts/build_language_font_coverage.py            # write the header
  python3 scripts/build_language_font_coverage.py --check    # fail if it is stale
  python3 scripts/build_language_font_coverage.py --report   # print the table
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

from font_cmap import CmapError, rendered_codepoints  # noqa: E402

TRANSLATIONS = ROOT / "lib" / "I18n" / "translations"
OUT = ROOT / "src" / "LanguageFontCoverage.h"

# Probe order is the answer to "what is the cheapest thing that fixes this
# codepoint", so it is also the mask each codepoint contributes.
# Mask bits, named once so the generator, the header and the test cannot drift apart.
NEED_CARRIER = 0x01       # the data-partition core tier covers it
NEED_WORLD_PACK = 0x02    # only the downloadable world-script pack covers it
NEED_CJK_PACK = 0x04      # only a wider downloadable CJK pack covers it
NEED_EMOJI_PACK = 0x08    # only the full emoji pack covers it
K_UNFIXABLE = 0x10
K_JOINING = 0x20
K_RTL = 0x40

LAYERS = (
    ("core3500", ROOT / "assets" / "fonts" / "CPYOSHanSansSC-Core.ttf", "kNeedCarrier", NEED_CARRIER),
    ("world", ROOT / "site" / "fonts" / "CPYOSWorldScripts.ttf", "kNeedWorldPack", NEED_WORLD_PACK),
    ("core8105", ROOT / "site" / "fonts" / "CPYOSCJKCore.ttf", "kNeedCjkPack", NEED_CJK_PACK),
    ("cjk_full", ROOT / "site" / "fonts" / "CPYOSCJK.ttf", "kNeedCjkPack", NEED_CJK_PACK),
    ("emoji", ROOT / "site" / "fonts" / "NotoEmoji-Full.ttf", "kNeedEmojiPack", NEED_EMOJI_PACK),
)
# app0 faces are inputs too, just not fixable layers: they define the residual.
APP0 = (
    ("tier0", ROOT / "assets" / "fonts" / "CPYOSHanSansSC-UI.ttf"),
    ("tier0b", ROOT / "assets" / "fonts" / "CPYOSWorldScripts-UI.ttf"),
)
ALL_ASSETS = dict(list(APP0) + [(name, path) for name, path, _const, _bit in LAYERS])

# Scripts whose text is rewritten before it is drawn.  lib/MiniBidi applies UAX#9
# reordering and Arabic contextual shaping (do_shape), so the codepoints the renderer
# finally asks for are presentation forms (U+FB50-U+FDFF, U+FE70-U+FEFF) that do NOT
# appear in the catalogues.  A static check against the YAML can therefore never certify
# these languages, and a font-extra download is not the fix either: the forms come from
# the built-in RTL face, which typography::apply() has to keep reachable.  See
# src/UiRtlFontId.h and scripts/tests/test_rtl_fallback.py.
# Scripts whose *drawn* codepoints differ from the stored ones: MiniBidi's do_shape
# rewrites Arabic letters into presentation forms (and Indic marks would need the same
# treatment), so no scan of the catalogues can certify them.
JOINING_RANGES = (
    (0x0600, 0x074F),  # Arabic + supplement
    (0x0750, 0x077F),  # Arabic Extended-A
    (0x08A0, 0x08FF),  # Arabic Extended-B
    (0x0900, 0x0DFF),  # Devanagari .. Gurmukhi (no catalogue uses these yet)
    (0xFB13, 0xFDFF),  # Armenian ligatures, Arabic presentation forms A
    (0xFE70, 0xFEFF),  # Arabic Presentation Forms B
)

# Scripts written right-to-left: MiniBidi's do_bidi handles the order, and the letters
# themselves stay in place, so this is informational rather than a refusal.  Hebrew is
# the case in this list that needs no shaping face.
RTL_RANGES = (
    (0x0590, 0x05FF),  # Hebrew
    (0x0600, 0x08FF),  # Arabic and its supplements
    (0xFB1D, 0xFEFC),  # presentation forms of both
)

_VALUE = re.compile(r'^([A-Za-z0-9_]+):\s*"(.*)"\s*$')
_META_KEYS = ("_language_name", "_language_code", "_order", "_bcp47")

# English names for the pre-install gate and the picker's fallback row.  LANGUAGE_NAMES
# holds endonyms ("العربية", "Беларуская") -- precisely the strings an uncovered language
# cannot draw -- so the gate needs Latin names.  These are English words, not
# translations, so they cost no i18n keys (the embedded EN blob has a 15-bit offset
# limit: AGENTS.md section 65).
EXONYMS = {
    "EN": "English", "AR": "Arabic", "AZ": "Azerbaijani", "BE": "Belarusian",
    "BG": "Bulgarian", "BS": "Bosnian", "CA": "Catalan", "CAV": "Valencian",
    "CS": "Czech", "DA": "Danish", "DE": "German", "ES": "Spanish", "FA": "Persian",
    "FI": "Finnish", "FR": "French", "HE": "Hebrew", "HU": "Hungarian",
    "ID": "Indonesian", "IT": "Italian", "KK": "Kazakh", "LT": "Lithuanian",
    "NB": "Norwegian", "NL": "Dutch", "OOK": "Orang Utu", "PL": "Polish",
    "PT": "Portuguese (Brazil)", "P2": "Portuguese (Portugal)", "RO": "Romanian",
    "RU": "Russian", "SK": "Slovak", "SI": "Slovenian", "SV": "Swedish",
    "TR": "Turkish", "UK": "Ukrainian", "VI": "Vietnamese", "ZH_CN": "Chinese",
}


def _yaml_values(path: pathlib.Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _VALUE.match(line.strip())
        if not m or m.group(1) in _META_KEYS:
            continue
        out[m.group(1)] = m.group(2).replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
    return out


def language_files() -> list[tuple[str, str, pathlib.Path]]:
    """(enum name, bcp47, path) in the order gen_i18n.py gives the Language enum.

    gen_i18n.py puts English first and sorts the rest by _bcp47.  The rule is
    duplicated here because the header is indexed by enum value and a silent reorder
    would hand every language another language's mask; the test compares against the
    enum actually emitted in I18nKeys.h rather than trusting either copy.
    """
    rows: list[tuple[str, str, pathlib.Path]] = []
    for path in sorted(TRANSLATIONS.glob("*.yaml")):
        text = path.read_text(encoding="utf-8")
        code = re.search(r'^_language_code:\s*"([A-Za-z0-9_]+)"', text, re.M)
        bcp = re.search(r'^_bcp47:\s*"([A-Za-z0-9-]+)"', text, re.M)
        if not code:
            raise SystemExit(f"{path.name}: no _language_code")
        rows.append((code.group(1).upper(), (bcp.group(1) if bcp else path.stem), path))
    rows.sort(key=lambda r: r[1])
    english = [r for r in rows if r[0] == "EN"]
    if len(english) != 1:
        raise SystemExit("exactly one translation must declare _language_code EN")
    return english + [r for r in rows if r[0] != "EN"]


def required_codepoints() -> dict[str, set[int]]:
    """Per enum name: every codepoint that language's UI can put on screen.

    A key the catalogue omits falls back to English at runtime (gen_i18n.py prints
    "using English fallback"), and nothing warns the user when those English glyphs are
    the ones missing -- so the English string's codepoints belong to this language too.
    """
    files = language_files()
    values = {name: _yaml_values(path) for name, _bcp, path in files}
    english = values["EN"]
    master = set(english)
    out: dict[str, set[int]] = {}
    for name, _bcp, _path in files:
        table = values[name]
        cps: set[int] = set()
        for key in master | set(table):
            cps.update(ord(c) for c in table.get(key, english.get(key, "")))
        out[name] = cps
    return out


def needs_layer(cp: int, layers: dict[str, set[int]]) -> int:
    for _name, _path, const, bit in LAYERS:
        if cp in layers[_name]:
            return bit
    return K_UNFIXABLE


def needs_shaping(cps: set[int]) -> bool:
    """True when the language's strings pass through the contextual-shaping pass.

    Measured on every required codepoint, not just the residual: app0 carries the Arabic
    letters these catalogues use, so the coverage arithmetic comes out at zero missing --
    while the text actually drawn is the shaped presentation forms, of which app0 carries
    none.  That gap is invisible to a catalogue scan, which is the whole reason this bit
    exists.  Hebrew is deliberately not here: its letters are drawn as stored.
    """
    return any(lo <= cp <= hi for cp in cps for lo, hi in JOINING_RANGES)


def is_rtl(cps: set[int]) -> bool:
    """True when the language's text runs right-to-left (do_bidi reorders it)."""
    return any(lo <= cp <= hi for cp in cps for lo, hi in RTL_RANGES)


def classify(residual: set[int], layers: dict[str, set[int]]) -> tuple[int, int]:
    """(mask, unfixable_count) for one language's residual."""
    mask = 0
    unfixable = 0
    for cp in residual:
        bit = needs_layer(cp, layers)
        mask |= bit
        if bit == K_UNFIXABLE:
            unfixable += 1
    return mask, unfixable


def compute() -> tuple[list[dict[str, object]], dict[str, set[int]]]:
    layers: dict[str, set[int]] = {}
    for name, path in ALL_ASSETS.items():
        try:
            # rendered_codepoints(), not the union of every subtable: this table decides
            # which languages the device blocks, so a legacy Mac Roman table whose slot means
            # a different character must not count as coverage. Measured on the current
            # assets the two are identical (none of them ships a (1,0) table), so this is a
            # basis correction, not a behaviour change.
            layers[name] = rendered_codepoints(path)
        except CmapError as exc:
            raise SystemExit(f"{path}: {exc}")
    app0 = layers["tier0"] | layers["tier0b"]
    required = required_codepoints()
    rows: list[dict[str, object]] = []
    for name, bcp, _path in language_files():
        cps = required[name]
        residual = {cp for cp in cps if cp > 0x7F and cp not in app0}
        mask, unfixable = classify(residual, layers)
        if needs_shaping(cps):
            mask |= K_JOINING
        if is_rtl(cps):
            mask |= K_RTL
        rows.append({
            "name": name,
            "bcp47": bcp,
            "exonym": EXONYMS.get(name, name),
            "total": len(cps),
            "residual": len(residual),
            "unfixable": unfixable,
            "mask": mask,
            "rtl": is_rtl(cps),
        })
    return rows, layers


CONST_BY_BIT = {
    NEED_CARRIER: "kNeedCarrier",
    NEED_WORLD_PACK: "kNeedWorldPack",
    NEED_CJK_PACK: "kNeedCjkPack",
    NEED_EMOJI_PACK: "kNeedEmojiPack",
    K_UNFIXABLE: "kUnfixable",
    K_JOINING: "kNeedsShapingFace",
    K_RTL: "kRtlScript",
}


def mask_label(mask: int) -> str:
    parts = [CONST_BY_BIT[bit] for bit in sorted(CONST_BY_BIT) if mask & bit]
    return "|".join(parts) if parts else "0"


def render(rows: list[dict[str, object]]) -> str:
    consts = "\n".join(f"constexpr uint8_t {CONST_BY_BIT[bit]} = 0x{bit:02X};"
                       for bit in sorted(CONST_BY_BIT))
    enum = "\n".join(f"  {str(row['name'])} = {i}," for i, row in enumerate(rows))
    entries = []
    for row in rows:
        # Comma before the comment: a trailing ", // note" puts the separator inside the
        # comment, which silently breaks the initialiser list.
        entries.append(
            f"    {{{int(row['mask'])}, {int(row['residual'])}, {int(row['unfixable'])}}},"
            f"  // {row['name']} / {row['bcp47']}: {int(row['residual'])} missing, "
            f"{int(row['unfixable'])} unfixable -> {mask_label(int(row['mask']))}"
        )
    exonyms = "\n".join(f'    "{row["exonym"]}",  // {row["name"]}' for row in rows)
    head = f"""#pragma once

// GENERATED by scripts/build_language_font_coverage.py -- do not edit by hand.
// Re-run it (or `--check` in CI) whenever lib/I18n/translations/*.yaml or a font asset
// changes.  Indexed by Language enum position; scripts/tests/
// test_language_font_coverage.py pins both the order (against I18nKeys.h) and every
// mask against an independent recomputation from the cmap data.

// Can this build draw language N?
//
//   mask        which layer would supply the missing glyphs, and whether a download is
//               enough at all (kNeedsShapingFace says it is not; kUnfixable says nothing)
//   missing     codepoints the language's strings need beyond Tier 0 + Tier 0b (app0)
//   unfixable   of those, the ones no committed font source contains
//
// A catalogue that omits a key falls back to English at runtime, so English strings are
// part of every language's requirement; that is why the codepoint counts below are
// larger than the language's own alphabet.

#include <cstdint>

namespace language_font {{

{consts}

// A download closes these. kNeedsShapingFace and kUnfixable are deliberately NOT in
// this set: for Arabic and Persian the glyphs that end up on screen are runtime-shaped
// presentation forms supplied by the built-in RTL face, not by a pack, so a download is
// not the answer (src/UiRtlFontId.h).  Hebrew is not in that bit -- its letters are
// drawn as stored and app0 covers them -- so it stays selectable.
constexpr uint8_t kDownloadableMask = kNeedCarrier | kNeedWorldPack | kNeedCjkPack |
                                      kNeedEmojiPack;

// Bits that mean "this table cannot certify the language from its catalogues".
constexpr uint8_t kRefuseMask = kNeedsShapingFace | kUnfixable;


struct Need {{
  uint8_t mask;
  uint16_t missing;    // codepoints beyond Tier 0 + Tier 0b (app0)
  uint16_t unfixable;  // of those, ones no committed font source contains
}};

// Language ids, in the order gen_i18n.py emits them (English first, rest by _bcp47).
enum Language : uint8_t {{
{enum}
  kCount
}};

inline constexpr Need kNeeds[] = {{
{chr(10).join(entries)}
}};

// English names, for rows an uncovered language cannot render in its own script.
inline constexpr const char* kEnglishNames[] = {{
{exonyms}
}};

static_assert(sizeof(kNeeds) / sizeof(kNeeds[0]) == kCount,
              "coverage table out of step with the enum");
static_assert(sizeof(kEnglishNames) / sizeof(kEnglishNames[0]) == kCount,
              "English names out of step with the enum");

constexpr const Need& needFor(uint8_t language) {{ return kNeeds[language]; }}

constexpr uint8_t maskFor(uint8_t language) {{ return kNeeds[language].mask; }}

constexpr const char* englishName(uint8_t language) {{ return kEnglishNames[language]; }}

// True when the blocking font gate should intercept this selection: something is
// missing, and installing it would actually help.
constexpr bool gateNeeded(uint8_t language) {{
  const uint8_t mask = maskFor(language);
  return (mask & kDownloadableMask) != 0 && (mask & kRefuseMask) == 0;
}}

// True when the selection must be refused outright (no pack can fix it).
constexpr bool mustRefuse(uint8_t language) {{ return (maskFor(language) & kRefuseMask) != 0; }}

constexpr bool isRtlScript(uint8_t language) {{ return (maskFor(language) & kRtlScript) != 0; }}

constexpr bool needsShapingFace(uint8_t language) {{
  return (maskFor(language) & kNeedsShapingFace) != 0;
}}

}}  // namespace language_font
"""
    return head


def print_report(rows, layers) -> None:
    app0 = layers["tier0"] | layers["tier0b"]
    print(f"app0: Tier0 {len(layers['tier0'])} + Tier0b {len(layers['tier0b'])} "
          f"= {len(app0)} codepoints")
    for name, _path, _const, _bit in LAYERS:
        print(f"  {name:10s} {len(layers[name]):6d}")
    print()
    width = max(len(str(r["name"])) for r in rows)
    for row in rows:
        print(f"{row['name']:<{width}} {row['bcp47']:<12} strings {row['total']:4d} "
              f"missing {row['residual']:4d} unfixable {row['unfixable']:4d}  "
              f"{mask_label(int(row['mask']))}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    rows, layers = compute()

    # The invariant that makes this table worth reading: everything the built-in copy
    # (35 catalogues, hard-coded literals, the almanac) can draw already lives in app0.
    from build_ui_font_subset import ui_codepoints  # stdlib-only; no fontTools import

    app0 = layers["tier0"] | layers["tier0b"]
    uncovered = ui_codepoints() - app0
    if uncovered:
        raise SystemExit("app0 cannot draw built-in UI codepoints: "
                         + ", ".join(f"U+{cp:04X}" for cp in sorted(uncovered)[:12]))

    if args.report:
        print_report(rows, layers)
        return
    text = render(rows)
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            raise SystemExit(f"{OUT} is stale; run scripts/build_language_font_coverage.py")
        print(f"language coverage up to date ({len(rows)} languages)")
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(rows)} languages)")


if __name__ == "__main__":
    main()
