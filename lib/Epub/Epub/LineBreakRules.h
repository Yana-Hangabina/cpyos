#pragma once

#include <cstdint>

// CJK line-breaking rules (禁则 / kinsoku) used by the reader's line breaker.
//
// Two prohibitions, both expressed as code-point predicates so the table is
// testable on the host without a renderer:
//   * a CLOSING punctuation mark may not start a line (it belongs to the line
//     it closes), so the breaker must not break *before* it;
//   * an OPENING punctuation mark may not end a line, so the breaker must not
//     break *after* it.
//
// `hangableAllowance()` additionally lets a closing mark hang past the right
// margin by half its advance instead of pushing the whole mark to the next
// line (标点悬挂). That matches what readers expect from typeset CJK text and
// costs nothing in the renderer: the glyph still draws at its normal position,
// the overflow simply lands in the page margin.
//
// Not implemented here: inter-punctuation compression (two consecutive marks
// sharing one em). It changes glyph advances, so it has to be applied by the
// renderer as well — tracked in the reader-UX plan, not done in this pass.
namespace LineBreakRules {

// Characters that must not begin a line: Chinese/Japanese closing marks, the
// small kana that attach to the preceding character, and their ASCII forms.
inline bool isLineStartForbidden(const uint32_t cp) {
  switch (cp) {
    // Full-width closing punctuation.
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF1F:  // ？
    case 0xFF01:  // ！
    case 0x3005:  // 々
    case 0x30FB:  // ・
    case 0x2026:  // …
    case 0x2025:  // ‥
    case 0x2014:  // —
    case 0x301C:  // 〜
    case 0xFF5E:  // ～
    case 0x30FC:  // ー
    // Closing brackets / quotes (CJK).
    case 0xFF09:  // ）
    case 0x3015:  // 〕
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3019:  // 〙
    case 0x3017:  // 〗
    case 0x301F:  // 〟
    case 0x201D:  // ”
    case 0x2019:  // ’
    case 0x301E:  // 〞
    // Small kana attach to the preceding character.
    case 0x3041:
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:
    case 0x3063:
    case 0x3083:
    case 0x3085:
    case 0x3087:
    case 0x308E:
    case 0x30A1:
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:
    case 0x30C3:
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:
    case 0x30EE:
    case 0x30F5:
    case 0x30F6:
    // ASCII forms.
    case ',':
    case '.':
    case ';':
    case ':':
    case '?':
    case '!':
    case ')':
    case ']':
    case '}':
      return true;
    default:
      return false;
  }
}

// Characters that must not end a line: opening brackets and quotes.
inline bool isLineEndForbidden(const uint32_t cp) {
  switch (cp) {
    case 0xFF08:  // （
    case 0x3014:  // 〔
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3018:  // 〘
    case 0x3016:  // 〖
    case 0x201C:  // “
    case 0x2018:  // ‘
    case 0x301D:  // 〝
    case '(':
    case '[':
    case '{':
      return true;
    default:
      return false;
  }
}

// How far a trailing mark may hang past the right margin, in pixels. Zero means
// "not hangable". Only closing marks hang; opening marks never sit at line end.
inline int hangableAllowance(const uint32_t cp, const int advance) {
  if (advance <= 0) return 0;
  if (!isLineStartForbidden(cp)) return 0;
  // Bracket-shaped marks and quotes read badly when they straddle the margin,
  // so only the small sentence marks hang.
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF1F:  // ？
    case 0xFF01:  // ！
    case 0x2026:  // …
    case 0x2014:  // —
    case ',':
    case '.':
    case ';':
    case ':':
    case '?':
    case '!':
      return advance / 2;
    default:
      return 0;
  }
}

}  // namespace LineBreakRules
