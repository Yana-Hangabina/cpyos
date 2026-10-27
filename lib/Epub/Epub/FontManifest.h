#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Pure helpers for EPUB-embedded-font discovery (A5). Kept dependency-free so
// they can be host-tested (see cpyos/src/font_manifest) without expat or the
// parser stack; ContentOpfParser and the reader both use them.
namespace epubfont {

// Media types that carry a loadable font file. The OPF spec's canonical value is
// application/font-sfnt; the others are what real EPUBs ship.
inline bool isFontMediaType(const std::string& mediaType) {
  return mediaType == "application/font-sfnt" || mediaType == "application/vnd.ms-opentype" ||
         mediaType == "application/x-font-ttf" || mediaType == "application/x-font-opentype" ||
         mediaType == "application/font-woff" || mediaType == "font/ttf" || mediaType == "font/otf" ||
         mediaType == "font/collection";
}

// True when the href ends in an extension the font loader can parse. Covers
// manifests that omit or mislabel the media type. Case-sensitive, as EPUB hrefs
// are; real packages use lower case.
inline bool hasFontExtension(const std::string_view href) {
  const auto endsWith = [&href](const char* ext) {
    const size_t n = std::char_traits<char>::length(ext);
    return href.size() >= n && href.compare(href.size() - n, n, ext) == 0;
  };
  return endsWith(".ttf") || endsWith(".otf") || endsWith(".ttc");
}

// A family name for an embedded font: the file stem (directory and extension
// stripped), or "epub" when nothing usable remains. Only a label for logs and the
// renderer's font-id hash -- the glyphs come from the file itself.
inline std::string familyFromHref(const std::string& href) {
  const size_t slash = href.find_last_of('/');
  std::string stem = (slash == std::string::npos) ? href : href.substr(slash + 1);
  const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem.resize(dot);
  if (stem.empty()) stem = "epub";
  return stem;
}

}  // namespace epubfont