#pragma once

#include <cstddef>
#include <string>

// Names the official FontsPage sends for SD-card font uploads. Kept as pure
// functions so traversal and odd extensions are rejected under host tests.
namespace plus_fonts {

inline bool safeFamily(const std::string& name) {
  if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') return false;
  }
  return name.front() != '.';
}

inline bool safeFilename(const std::string& name) {
  if (name.empty() || name.size() > 96 || name.front() == '.') return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') return false;
  }
  auto ends = [&](const char* suffix) {
    const std::size_t len = std::char_traits<char>::length(suffix);
    if (name.size() <= len) return false;
    for (std::size_t i = 0; i < len; ++i) {
      const char c = name[name.size() - len + i];
      const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      if (lower != suffix[i]) return false;
    }
    return true;
  };
  return ends(".ttf") || ends(".otf") || ends(".ttc") || ends(".cpfont");
}

}  // namespace plus_fonts
