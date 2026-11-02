#pragma once

// Unified firmware: compact international UI fonts are primary, with the
// built-in Simplified-Chinese subsets registered as missing-glyph fallbacks.
// Broad reader families and larger CJK sizes are supplied as .cpfont files.
#include <builtinFonts/chinese_chess_16.h>
#include <builtinFonts/notosans_8_regular.h>
#if defined(CROSSPOINT_SYSTEM_FONT_PACK)
// The external pack carries every UI size. Keep the 8pt subset only as the
// last-resort fallback if the pack is missing (app-only OTA on an old device):
// a readable UI is better than tofu, and 341 KB is the cheapest insurance.
#include <builtinFonts/notosans_cjk_8.h>
#else
#include <builtinFonts/notosans_cjk_8.h>
#include <builtinFonts/notosans_cjk_10.h>
#ifndef CROSSPOINT_EXTERNAL_FONTS
#include <builtinFonts/notosans_cjk_12.h>
#endif
#endif
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>
