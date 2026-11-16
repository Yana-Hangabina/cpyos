// CFF ink-scale regression for the device's FreeType amalgamation.
//
// The vendored FreeType does not link the `pshinter' module. cff_slot_init()
// then cannot set glyph_hints, the Type 2 interpreter scales the outline, and
// cff_slot_load scales it a second time (ink ~x_scale^2, advance x_scale^1).
// The fix disables hinting when no hinter is available, so the hinted and the
// explicitly unhinted load must now produce the same ink box and advance.
//
// usage: CffSizeHostTest <font> [font-b] [pixel-size]
//   one font  : hinted-vs-unhinted self check (fails on the double scale)
//   two fonts : also compares the ink height of two faces of the same family
// exit 0 = all checks pass.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <stdio.h>
#include <stdlib.h>

static int renderInk(const char* path, int px, const unsigned long* cps, int n, FT_Int32 flags, int* w, int* h,
                     int* adv) {
  FT_Library lib;
  if (FT_Init_FreeType(&lib)) return -1;
  FT_Add_Default_Modules(lib);
  FT_Face face;
  if (FT_New_Face(lib, path, 0, &face)) return -2;
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px)) return -3;
  *w = *h = *adv = 0;
  for (int i = 0; i < n; ++i) {
    if (FT_Get_Char_Index(face, cps[i]) == 0) continue;
    if (FT_Load_Char(face, cps[i], flags)) return -4;
    if (i == 3) {
      FT_BBox box;
      FT_Outline_Get_CBox(&face->glyph->outline, &box);
      printf("  probe: upem=%u x_scale=%ld cbox=%ld,%ld..%ld,%ld advance=%.1fpx\n", (unsigned)face->units_per_EM,
             (long)face->size->metrics.x_scale, (long)box.xMin, (long)box.yMin, (long)box.xMax, (long)box.yMax,
             face->glyph->advance.x / 64.0);
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return -5;
    if (face->glyph->bitmap.width > (unsigned)*w) *w = (int)face->glyph->bitmap.width;
    if (face->glyph->bitmap.rows > (unsigned)*h) *h = (int)face->glyph->bitmap.rows;
    if ((face->glyph->advance.x >> 6) > *adv) *adv = (int)(face->glyph->advance.x >> 6);
  }
  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  return (*w > 0 && *h > 0) ? 0 : -6;
}

static const unsigned long kCodepoints[] = {0x4F60, 0x597D, 0x6211, 'A', 'g'};

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <font> [font-b] [px]\n", argv[0]);
    return 2;
  }
  const int px = argc > 3 ? atoi(argv[3]) : 32;

  int hw = 0, hh = 0, ha = 0, uw = 0, uh = 0, ua = 0;
  if (renderInk(argv[1], px, kCodepoints, 5, FT_LOAD_DEFAULT, &hw, &hh, &ha)) return 1;
  printf("%s hinted: %dx%d ink, advance %d @%dpx\n", argv[1], hw, hh, ha, px);
  if (renderInk(argv[1], px, kCodepoints, 5, FT_LOAD_NO_HINTING, &uw, &uh, &ua)) return 1;
  printf("%s unhinted: %dx%d ink, advance %d @%dpx\n", argv[1], uw, uh, ua, px);

  // A real hinter may move a row/column; a double scale cannot (it was ~2.1x).
  if (hh > uh + 2 || hw > uw + 2 || abs(ha - ua) > 1) {
    fprintf(stderr, "FAIL: hinted ink %dx%d/adv %d disagrees with unhinted %dx%d/adv %d (double scale?)\n", hw, hh, ha,
            uw, uh, ua);
    return 1;
  }
  printf("hinted/unhinted parity OK\n");

  if (argc > 2) {
    int bw = 0, bh = 0, ba = 0;
    if (renderInk(argv[2], px, kCodepoints, 5, FT_LOAD_DEFAULT, &bw, &bh, &ba)) return 1;
    const double ratio = (double)hh / (double)bh;
    printf("%s: %dx%d ink, advance %d; ink height ratio a/b = %.2f, advance ratio = %.2f\n", argv[2], bw, bh, ba, ratio,
           (double)ha / (double)ba);
    if (ratio > 1.3 || ratio < 0.7) {
      fprintf(stderr, "FAIL: CFF/glyf ink height ratio %.2f outside 0.7..1.3\n", ratio);
      return 1;
    }
  }
  printf("CFF ink checks OK\n");
  return 0;
}
