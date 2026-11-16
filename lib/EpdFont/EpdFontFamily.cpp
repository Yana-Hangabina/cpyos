#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  const EpdGlyph* glyph = font->getGlyph(cp);
  // A styled face that is missing this glyph (a bold/italic face that failed
  // to fault under memory pressure, or a font with no bold design) must not
  // turn the run into missing-glyph boxes while the regular design can draw
  // it. Emphasis is preserved by the synthetic-bold path.
  if (glyph == nullptr && font != regular) glyph = regular->getGlyph(cp);
  return glyph;
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style, bool* const usedReplacement) const {
  const EpdFont* font = getFont(style);
  bool missing = false;
  const EpdGlyph* glyph = font->getGlyph(cp, &missing);
  if (glyph == nullptr && font != regular) {
    bool missingInRegular = false;
    glyph = regular->getGlyph(cp, &missingInRegular);
    if (glyph != nullptr) {
      if (usedReplacement) *usedReplacement = missingInRegular;
      return glyph;
    }
  }
  if (usedReplacement) *usedReplacement = missing;
  return glyph;
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  return font->hasCodepoint(cp) || (font != regular && regular->hasCodepoint(cp));
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
