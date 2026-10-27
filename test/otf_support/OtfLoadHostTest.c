// Host test for the FreeInkFont FreeType build the device compiles: an OpenType
// face with PostScript outlines (CFF) must load and rasterize real coverage
// exactly like the TrueType path does. Cross-checked with a plain TrueType
// fixture so the CFF driver addition cannot silently regress the tt path.
//
// usage: OtfLoadHostTest <font> [expect-cff]
//   exit 0 = loaded and produced coverage; 1 = expected failure or rejection.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_TRUETYPE_TABLES_H

#include <stdio.h>
#include <string.h>

static int cffRegistered(FT_Library lib) {
  // FT_Get_First_Module is internal; probe the shared library instead: ask a
  // face whether the cff interpreter honors its property. Missing modules make
  // FT_Property_Set return an error.
  FT_Bool noStemDarkening = 1;
  return FT_Property_Set(lib, "cff", "no-stem-darkening", &noStemDarkening) == 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [expect-cff]\n", argv[0]);
    return 2;
  }
  const int expectCff = argc > 2 && argv[2][0] == '1';

  FT_Library lib;
  if (FT_Init_FreeType(&lib)) {
    fprintf(stderr, "FT_Init_FreeType failed\n");
    return 1;
  }
  FT_Add_Default_Modules(lib);

  const int haveCff = cffRegistered(lib);
  printf("cff module registered: %s\n", haveCff ? "yes" : "no");
  if (expectCff && !haveCff) {
    fprintf(stderr, "FAIL: cff driver not registered in a CFF build\n");
    return 1;
  }

  FT_Face face;
  const FT_Error err = FT_New_Face(lib, argv[1], 0, &face);
  if (err) {
    fprintf(stderr, "FT_New_Face error 0x%02x (face rejected)\n", err);
    return 1;
  }
  printf("face: family=%s glyphs=%ld\n", face->family_name, face->num_glyphs);

  // Same acceptance criteria SdCardFontRegistry applies when it lists a loose
  // font (FtFont::supportedFace): scalable outlines + a Unicode cmap. Error,
  // not just mark, if a CFF face ever fails these — the registry would drop it
  // before the driver ever gets a chance to render.
  if (!FT_IS_SCALABLE(face)) {
    fprintf(stderr, "FAIL: face is not scalable (registry would reject it)\n");
    return 1;
  }
  if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
    fprintf(stderr, "FAIL: no Unicode cmap (registry would reject it)\n");
    return 1;
  }
  if (FT_Get_Sfnt_Table(face, FT_SFNT_OS2) == NULL) {
    printf("note: OS/2 absent — registry weight refinement falls back to filename\n");
  }

  if (FT_Set_Pixel_Sizes(face, 0, 32)) {
    fprintf(stderr, "FAIL: FT_Set_Pixel_Sizes\n");
    return 1;
  }

  // ASCII always fits the fixture; CJK points are checked when the cmap has
  // them (the tiny fixture maps a few) and skipped otherwise.
  const unsigned long cps[] = {'A', 'g', '0', 0x4F60, 0x597D};
  unsigned checked = 0;
  for (unsigned i = 0; i < sizeof(cps) / sizeof(cps[0]); ++i) {
    const unsigned long cp = cps[i];
    const FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) {
      printf("U+%04lX: missing (skip)\n", cp);
      continue;
    }
    if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
      fprintf(stderr, "FAIL: rasterize U+%04lX\n", cp);
      return 1;
    }
    unsigned covered = 0;
    for (unsigned y = 0; y < (unsigned)face->glyph->bitmap.rows; ++y) {
      for (unsigned x = 0; x < (unsigned)face->glyph->bitmap.width; ++x) {
        if (face->glyph->bitmap.buffer[y * face->glyph->bitmap.pitch + x]) ++covered;
      }
    }
    printf("U+%04lX: glyph=%u size=%dx%d coverage=%u\n", cp, gi, face->glyph->bitmap.width,
           face->glyph->bitmap.rows, covered);
    if (covered == 0) {
      fprintf(stderr, "FAIL: U+%04lX rasterized empty\n", cp);
      return 1;
    }
    ++checked;
  }
  if (checked < 2) {
    fprintf(stderr, "FAIL: too few glyphs checked (%u)\n", checked);
    return 1;
  }

  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  printf("PASS\n");
  return 0;
}
