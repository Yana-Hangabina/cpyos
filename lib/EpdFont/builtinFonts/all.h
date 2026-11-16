#pragma once

// Unified firmware: compact international UI fonts are primary, with the
// built-in Simplified-Chinese subsets registered as missing-glyph fallbacks.
// Broad reader families and larger CJK sizes are supplied as .cpfont files.
#include <builtinFonts/chinese_chess_16.h>
#include <builtinFonts/notosans_8_regular.h>
#if defined(CROSSPOINT_SYSTEM_FONT_PACK)
// Embedded builds carry the UI subset in app0 and register the vector face at
// 8pt too, so the 3,500-glyph bitmap is unreachable and not linked (it cost
// 341 KB). Spiffs builds (C3) keep it: their pack can be absent after an
// app-only OTA, and a readable 8pt UI beats tofu.
#if !defined(CROSSPOINT_FONT_PACK_EMBEDDED)
#include <builtinFonts/notosans_cjk_8.h>
#endif
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
