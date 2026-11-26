#pragma once

// The built-in right-to-left / shaping face id.
//
// Two facts make this id load-bearing rather than cosmetic:
//
//  1. lib/MiniBidi applies UAX#9 reordering *and* Arabic contextual shaping
//     (do_shape, plus Lam-Alef ligation).  Shaping rewrites ordinary letters into
//     Arabic Presentation Forms (U+FB50-U+FDFF, U+FE70-U+FEFF), so the codepoints the
//     renderer finally asks for are *not* the codepoints in lib/I18n/translations/*.yaml.
//     A static coverage check on the catalogues cannot see them -- which is precisely how
//     an Arabic UI can look "covered" (all 54 letters present in app0) and still box.
//  2. Neither app0 face carries those forms (measured: 0 codepoints in U+FB50-U+FDFF and
//     U+FE70-U+FEFF across CPYOSHanSansSC-UI.ttf + CPYOSWorldScripts-UI.ttf), while the
//     RTL family does: lib/EpdFont/builtinFonts/ubuntu_12_regular.h was built from
//     Ubuntu + NotoSansHebrew + NotoSansArabic with --additional-intervals covering
//     0xFB56-0xFB59, 0xFB66-0xFB69, 0xFB7A-0xFB7D, 0xFB88-0xFB95, 0xFB9E-0xFB9F,
//     0xFBA6-0xFBB1, 0xFBFC-0xFBFF and 0xFE80-0xFEFC.  It is the only face in the image
//     that can draw shaped Arabic, and Hebrew comes from the same build.
//
// main.cpp registers the family under this id and points CJK_UI_12 at it.  That fallback
// is wiped by SystemFontPack::addFaceId(), which calls removeFont() on the role ids it
// rebinds (GfxRenderer::removeFont erases fallback entries keyed by the id), so the
// binding has to be re-applied wherever the roles are re-pointed -- see
// typography::apply().
//
// scripts/tests/test_rtl_fallback.py fails if main.cpp's literal stops matching this
// constant, and if the fallback is ever dropped again.  Change both places together.
namespace ui_rtl {

inline constexpr int kFontId = 0x52544C0C;

}  // namespace ui_rtl
