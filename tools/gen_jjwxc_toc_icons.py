#!/usr/bin/env python3
"""Generate src/components/icons/jjwxcTocIcons.h — the JJWXC chapter-list tail art.

Two families, both packed in the freeink::Icon convention (MSB-first, bit 1 =
leave the pixel / transparent, bit 0 = draw ink; see Icon.h):

  * State glyphs — FREE / VIP / VIP+ (yen) / LOCK with NO outer frame: the
    row draws one rounded square button around the glyph, like the library's
    sync button. Low density 24x24 (5x9 font), high density 32x32 (6x12 font).

  * Action glyphs — Lucide `download` / `refresh-cw` at 24 / 32 px, the same
    icon box as the state glyph inside the button. The SDK generator's rasters
    are embedded so this header stays reproducible without rsvg-convert/Pillow:
        python3 <sdk>/libs/assets/Icons/tools/gen_icons.py \
            --manifest /tmp/jj_actions.manifest \
            --svgdir /tmp/lucide_svg --sizes 24,32 --out /tmp/jj_actions.h

The badges are outlines only: everything except border + glyph is left
transparent, so the row behind them shows through (paper on a normal row,
black on an inverted cursor row). Draw with Paint::solid(Color::Black) on a
normal row and Paint::solid(Color::White) on a selected/inverted row.

Run:  python3 tools/gen_jjwxc_toc_icons.py
"""

import os

# --- pixel fonts -----------------------------------------------------------
# 5x9, 1 px stroke. Cap height 9, advance 5 + 1 tracking. Nine rows (not seven)
# because the word has to hold a square: 7 rows read as a thin strip marooned
# in a 29 px frame.
FONT_5x9 = {
    "F": ["#####", "#....", "#....", "#....", "####.", "#....", "#....", "#....", "#...."],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", "#...#", "#...#"],
    "E": ["#####", "#....", "#....", "#....", "####.", "#....", "#....", "#....", "#####"],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.."],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#....", "#....", "#...."],
    "\u00a5": ["#...#", ".#.#.", "..#..", "..#..", "#####", "..#..", "#####", "..#..", "..#.."],
}

# 6x12, 2 px stems (the yen crossbars stay 2 px; at this weight two 2 px bars
# with 1 px of air between them would smear into a blob at 1x, so they get 2 px
# of air). Cap height 12 / badge 40 keeps the same 31% ink height as 9 / 29.
FONT_6x12 = {
    "F": ["######", "######", "##....", "##....", "##....", "######", "######", "##....", "##....", "##....", "##....", "##...."],
    "R": ["#####.", "#####.", "##..##", "##..##", "##..##", "#####.", "#####.", "##.##.", "##.##.", "##.##.", "##..##", "##..##"],
    "E": ["######", "######", "##....", "##....", "##....", "#####.", "#####.", "##....", "##....", "##....", "######", "######"],
    "V": ["##..##", "##..##", "##..##", "##..##", "##..##", "##..##", ".####.", ".####.", ".####.", "..##..", "..##..", "..##.."],
    "I": ["######", "######", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "######", "######"],
    "P": ["#####.", "#####.", "##..##", "##..##", "##..##", "#####.", "#####.", "##....", "##....", "##....", "##....", "##...."],
    "\u00a5": ["##..##", "##..##", ".####.", "..##..", "..##..", "######", "######", "..##..", "..##..", "######", "######", "..##.."],
}

# Padlock, 1 px stroke (15x15) and 2 px stroke (22x22). Body + shackle + keyhole;
# reads as "locked" in both languages, so no translated word is needed.
LOCK_15 = [
    "....#######....",
    "...#.......#...",
    "...#.......#...",
    "...#.......#...",
    "..###########..",
    "..#.........#..",
    "..#.........#..",
    "..#....#....#..",
    "..#...###...#..",
    "..#....#....#..",
    "..#.........#..",
    "..#.........#..",
    "..#.........#..",
    "..#.........#..",
    "..###########..",
]

LOCK_22 = [
    "......##########......",
    "....##############....",
    "....##..........##....",
    "....##..........##....",
    "....##..........##....",
    "....##..........##....",
    "....##..........##....",
    "....##..........##....",
    ".####################.",
    ".####################.",
    ".##................##.",
    ".##................##.",
    ".##......####......##.",
    ".##......####......##.",
    ".##.....######.....##.",
    ".##.....######.....##.",
    ".##.....######.....##.",
    ".##.....######.....##.",
    ".##......####......##.",
    ".##................##.",
    ".####################.",
    ".####################.",
]

# --- raster helpers --------------------------------------------------------


def canvas(size):
    return [[0] * size for _ in range(size)]


def _ring(size, inset, radius):
    """Outline pixels of a rounded square (1 px stroke), as (x, y) pairs."""
    lo, hi = inset, size - 1 - inset
    pts = set()
    for x in range(lo + radius, hi - radius + 1):
        pts.add((x, lo))
        pts.add((x, hi))
    for y in range(lo + radius, hi - radius + 1):
        pts.add((lo, y))
        pts.add((hi, y))
    for k in range(1, radius):
        pts.add((lo + k, lo + radius - k))
        pts.add((hi - k, lo + radius - k))
        pts.add((lo + k, hi - radius + k))
        pts.add((hi - k, hi - radius + k))
    return pts


def draw_border(cv, stroke, radius):
    """A `stroke`-px band made of nested 1 px outlines — a 45-degree corner
    staircase, the same optical rounding Lucide's 24 px squares use."""
    n = len(cv)
    for inset in range(stroke):
        for x, y in _ring(n, inset, max(0, radius - inset)):
            cv[y][x] = 1


def blit(cv, art, x0, y0):
    for dy, row in enumerate(art):
        for dx, ch in enumerate(row):
            if ch == "#":
                cv[y0 + dy][x0 + dx] = 1


def draw_word(cv, word, font, x0, y0, tracking):
    x = x0
    for ch in word:
        glyph = font[ch]
        for dy, row in enumerate(glyph):
            for dx, cell in enumerate(row):
                if cell == "#":
                    cv[y0 + dy][x + dx] = 1
        x += len(glyph[0]) + tracking


def word_width(word, font, tracking):
    return sum(len(font[ch][0]) for ch in word) + tracking * (len(word) - 1)


def pack_mask1(cv):
    """Mask1: MSB-first, bit 1 = transparent, bit 0 = ink."""
    n = len(cv)
    stride = (n + 7) // 8
    out = []
    for y in range(n):
        for xb in range(0, n, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                ink = x < n and cv[y][x] == 1
                byte |= (0 if ink else 1) << (7 - b)
            out.append(byte)
    assert len(out) == stride * n
    return out


def optical_center(cv):
    """Ink centroid, used for the Lucide action glyphs exactly as gen_icons.py
    computes it. Badges override this with their geometric center: a bordered
    square is aligned as a box, and its asymmetries (a keyhole, a short word)
    must not pull the frame off the row's center line."""
    total = 0
    count = 0
    for y, row in enumerate(cv):
        for cell in row:
            if cell:
                total += y
                count += 1
    return round(total / count) if count else len(cv) // 2


def ascii_art(cv):
    return "\n".join("".join("#" if cell else "." for cell in row) for row in cv)


# --- badge builders --------------------------------------------------------

BADGES = {
    # key: (size, font, tracking, word or art). No border: the row draws one
    # rounded button frame around the glyph, exactly like the library's sync
    # button, so an inner badge frame would read as a box inside a box.
    "FREE": (24, FONT_5x9, 1, "FREE"),
    "VIP": (24, FONT_5x9, 1, "VIP"),
    "VIPYEN": (24, FONT_5x9, 1, "VIP\u00a5"),
    "LOCK": (24, None, 0, LOCK_15),
    "FREE_HD": (32, FONT_6x12, 2, "FREE"),
    "VIP_HD": (32, FONT_6x12, 2, "VIP"),
    "VIPYEN_HD": (32, FONT_6x12, 2, "VIP\u00a5"),
    "LOCK_HD": (32, None, 0, LOCK_22),
}


def build_badge(spec):
    size, font, tracking, art = spec
    cv = canvas(size)
    if font is not None:
        width = word_width(art, font, tracking)
        glyph_h = len(font[art[0]])
        x = (size - width) // 2
        y = (size - glyph_h) // 2
        draw_word(cv, art, font, x, y, tracking)
    else:
        glyph_h = len(art)
        glyph_w = len(art[0])
        x = (size - glyph_w) // 2
        y = (size - glyph_h) // 2
        blit(cv, art, x, y)
    return cv


# --- action glyphs ---------------------------------------------------------

# Rasterised by the SDK generator from Lucide `download` / `refresh-cw` at
# 24 px (low density) and 36 px (high density); opticalCenterY is the ink
# centroid the generator baked in. Regenerate with:
#   gen_icons.py --manifest ... --svgdir <lucide/icons> --sizes 24,36
# Download keeps its tray: after the tail slot fill removed the old text
# bleed, the tray reads as a download glyph, not as stray dots, and it
# matches refresh-cw's visual weight at the same size.
ACTIONS = {
    "download_24": (
        14,
        [
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF,
            0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF, 0xFC, 0xE7, 0x3F, 0xFC, 0x66, 0x3F, 0xFE, 0x24, 0x7F,
            0xFF, 0x00, 0xFF, 0xFF, 0x81, 0xFF, 0xCF, 0xC3, 0xF3, 0xCF, 0xE7, 0xF3, 0xCF, 0xFF, 0xF3, 0xCF, 0xFF, 0xF3,
            0xCF, 0xFF, 0xF3, 0xCF, 0xFF, 0xF3, 0xE0, 0x00, 0x07, 0xF0, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        ],
    ),
    "refresh_cw_24": (
        12,
        [
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0xF3, 0xFE, 0x00, 0x73, 0xFC, 0x7E, 0x13, 0xF9, 0xFF, 0x83,
            0xF3, 0xFF, 0xC3, 0xE7, 0xFE, 0x03, 0xE7, 0xFE, 0x03, 0xCF, 0xFF, 0xFF, 0xCF, 0xFF, 0xFF, 0xCF, 0xFF, 0xF3,
            0xCF, 0xFF, 0xF3, 0xFF, 0xFF, 0xF3, 0xFF, 0xFF, 0xF3, 0xC0, 0x7F, 0xE7, 0xC0, 0x7F, 0xE7, 0xC3, 0xFF, 0xCF,
            0xC1, 0xFF, 0x9F, 0xC8, 0x7E, 0x3F, 0xCE, 0x00, 0x7F, 0xCF, 0x81, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        ],
    ),
    "download_32": (
        20,
        [
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE,
            0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF,
            0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0x3E, 0x7C, 0xFF, 0xFF, 0x1E,
            0x78, 0xFF, 0xFF, 0x8E, 0x71, 0xFF, 0xFF, 0xC6, 0x63, 0xFF, 0xFF, 0xE0, 0x07, 0xFF, 0xFF, 0xF0, 0x0F, 0xFF,
            0xFF, 0xF8, 0x1F, 0xFF, 0xE7, 0xFC, 0x3F, 0xE7, 0xE7, 0xFE, 0x7F, 0xE7, 0xE7, 0xFF, 0xFF, 0xE7, 0xE7, 0xFF,
            0xFF, 0xE7, 0xE7, 0xFF, 0xFF, 0xE7, 0xE7, 0xFF, 0xFF, 0xE7, 0xE7, 0xFF, 0xFF, 0xE7, 0xE3, 0xFF, 0xFF, 0xC7,
            0xE0, 0x00, 0x00, 0x07, 0xF0, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF,
        ],
    ),
    "refresh_cw_32": (
        15,
        [
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xE7, 0xFF, 0x80,
            0x01, 0xE7, 0xFF, 0x03, 0xC0, 0xE7, 0xFE, 0x1F, 0xF8, 0x27, 0xFC, 0x3F, 0xFE, 0x07, 0xF8, 0xFF, 0xFF, 0x07,
            0xF0, 0xFF, 0xF8, 0x07, 0xF1, 0xFF, 0xF0, 0x07, 0xF3, 0xFF, 0xF0, 0x07, 0xE3, 0xFF, 0xFF, 0xFF, 0xE3, 0xFF,
            0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xE7, 0xE7, 0xFF, 0xFF, 0xE7, 0xFF, 0xFF, 0xFF, 0xE7,
            0xFF, 0xFF, 0xFF, 0xC7, 0xFF, 0xFF, 0xFF, 0xC7, 0xE0, 0x0F, 0xFF, 0xCF, 0xE0, 0x0F, 0xFF, 0x8F, 0xE0, 0x1F,
            0xFF, 0x1F, 0xE0, 0xFF, 0xFF, 0x1F, 0xE0, 0x7F, 0xFC, 0x3F, 0xE4, 0x1F, 0xF8, 0x7F, 0xE7, 0x03, 0xC0, 0xFF,
            0xE7, 0x80, 0x03, 0xFF, 0xE7, 0xF0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF,
        ],
    ),
}

NAME = {
    "FREE": ("BadgeFree", 24),
    "VIP": ("BadgeVip", 24),
    "VIPYEN": ("BadgeVipYen", 24),
    "LOCK": ("BadgeLock", 24),
    "FREE_HD": ("BadgeFreeHd", 32),
    "VIP_HD": ("BadgeVipHd", 32),
    "VIPYEN_HD": ("BadgeVipYenHd", 32),
    "LOCK_HD": ("BadgeLockHd", 32),
}


def emit_array(name, data, width):
    lines = [f"inline constexpr uint8_t {name}Bits[] = {{"]
    per = 12
    for i in range(0, len(data), per):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + per])
        lines.append("    " + chunk + ",")
    lines.append("};")
    lines.append(f"inline constexpr freeink::Icon {name} = {{{width}, {width}, __CENTER__, {name}Bits}};")
    return "\n".join(lines)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, "src", "components", "icons", "jjwxcTocIcons.h")

    parts = []
    parts.append("""#pragma once

// JJWXC chapter-list tail artwork: state badges (FREE / VIP / VIP+yen / LOCK)
// and the two action glyphs (download / refresh-cw) at their draw sizes.
//
// Everything here is freeink::Icon data: MSB-first rows, (w + 7) / 8 bytes per
// row, bit 1 = leave the pixel, bit 0 = draw ink (Icon.h). The badge interiors
// are transparent, so the row behind them shows through: draw them with
// fui::bitmapFromIcon() (BitmapFormat::Mask1) and
//   Paint::solid(Color::Black)  on a normal row,
//   Paint::solid(Color::White)  on a selected (inverted) row.
// BitmapMode::Center draws them 1:1; Contain/Stretch would resample the thin
// 1 px border into broken pixels.
//
// Regenerate with: python3 tools/gen_jjwxc_toc_icons.py
// State glyphs are borderless: the row's rounded button frame supplies the
// outline. Action glyphs are the SDK generator's Lucide download / refresh-cw
// rasters at 24 px (low) and 32 px (high).
//
// Icon boxes: 24x24 (5x9 font) low, 32x32 (6x12 font) high; the row places
// each inside its own 64 px (high) / 44 px (low) rounded button.

#include "Icon.h"

namespace jjwxc_toc {
""")

    for key, spec in BADGES.items():
        cv = build_badge(spec)
        data = pack_mask1(cv)
        center = len(cv) // 2  # geometric center: badges align as boxes
        name, width = NAME[key]
        parts.append(f"// {key.replace('_HD', '').replace('YEN', '')}  {width}x{width}")
        parts.append("//")
        for line in ascii_art(cv).splitlines():
            parts.append("//   " + line)
        parts.append("//")
        parts.append(emit_array(name, data, width).replace("__CENTER__", str(center)))
        parts.append("")

    for key, (center, data) in ACTIONS.items():
        name = "".join(part.capitalize() for part in key.split("_"))
        size = int(key.rsplit("_", 1)[1])
        parts.append(f"// {key.replace('_', ' ')}  {size}x{size}")
        parts.append(emit_array(name, data, size).replace("__CENTER__", str(center)))
        parts.append("")

    parts.append("""// Selectors, so callers never spell a size or a state twice.
enum class Badge : uint8_t { Free, Vip, VipYen, Lock };
enum class Action : uint8_t { None, Download, Refresh };
enum class Density : uint8_t { Low, High };

inline const freeink::Icon& badgeIcon(const Density density, const Badge badge) {
  const bool hd = density == Density::High;
  switch (badge) {
    case Badge::VipYen:
      return hd ? BadgeVipYenHd : BadgeVipYen;
    case Badge::Lock:
      return hd ? BadgeLockHd : BadgeLock;
    case Badge::Vip:
      return hd ? BadgeVipHd : BadgeVip;
    case Badge::Free:
    default:
      return hd ? BadgeFreeHd : BadgeFree;
  }
}

// nullptr for Action::None, so a caller that has no action cannot draw one by
// accident (a badge-only row must leave the action column empty).
inline const freeink::Icon* actionIcon(const Density density, const Action action) {
  const bool hd = density == Density::High;
  switch (action) {
    case Action::Refresh:
      return hd ? &RefreshCw32 : &RefreshCw24;
    case Action::Download:
      return hd ? &Download32 : &Download24;
    case Action::None:
    default:
      return nullptr;
  }
}

// Badge extent and the gap to the action glyph, in pixels.
inline constexpr int16_t badgeSize(const Density density) { return density == Density::High ? 32 : 24; }
inline constexpr int16_t actionSize(const Density density) { return density == Density::High ? 32 : 24; }
// The row places the two icons in separate square buttons; this is only
// the gap inside a single button around its glyph.
inline constexpr int16_t badgeActionGap(const Density density) { return 0; }
}  // namespace jjwxc_toc""")

    text = "\n".join(parts)
    with open(out_path, "w") as handle:
        handle.write(text)
    print(f"wrote {out_path}")

    # Console previews for review.
    for key, spec in BADGES.items():
        cv = build_badge(spec)
        print(f"\n== {key} ({len(cv)}x{len(cv)}) center={optical_center(cv)}")
        print(ascii_art(cv))


if __name__ == "__main__":
    main()
