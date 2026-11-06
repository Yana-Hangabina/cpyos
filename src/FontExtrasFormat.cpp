#include "FontExtrasFormat.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace font_extras {
namespace {

constexpr size_t kPackFields = 9;
constexpr size_t kMaxLineBytes = 1024;

bool copyField(char* dst, size_t cap, const std::string_view src, const size_t maxLen) {
  if (src.empty() || src.size() > maxLen || src.size() >= cap) return false;
  std::memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
  return true;
}

bool asciiAlphaNum(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool hasControl(const std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
  }
  return false;
}

bool hasControlOrSpace(const std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return true;
  }
  return false;
}

bool endsWithIgnoreCase(const std::string_view text, const std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  const size_t at = text.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[at + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

bool parseSize(const std::string_view text, uint64_t& out) {
  if (text.empty() || text.size() > 12) return false;
  for (const char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  char buffer[16];
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = '\0';
  char* end = nullptr;
  const unsigned long long value = std::strtoull(buffer, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  if (value < 512 || value > kMaxPackBytes) return false;
  out = static_cast<uint64_t>(value);
  return true;
}

void setError(char* error, const size_t errorCap, const size_t line, const char* reason) {
  if (error == nullptr || errorCap == 0) return;
  if (line == 0) {
    std::snprintf(error, errorCap, "%s", reason);
  } else {
    std::snprintf(error, errorCap, "line %u: %s", static_cast<unsigned>(line), reason);
  }
}

std::vector<std::string_view> splitTabs(const std::string_view line) {
  std::vector<std::string_view> fields;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      fields.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return fields;
}

}  // namespace

bool parseSha256Hex(const char* text, uint8_t out[32]) {
  if (text == nullptr || out == nullptr || std::strlen(text) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    int hi = -1;
    int lo = -1;
    for (int half = 0; half < 2; ++half) {
      const char c = text[i * 2 + static_cast<size_t>(half)];
      int value = -1;
      if (c >= '0' && c <= '9') {
        value = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
      }
      if (value < 0) return false;
      if (half == 0) {
        hi = value;
      } else {
        lo = value;
      }
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool isValidId(const char* text) {
  if (text == nullptr || *text == '\0' || std::strlen(text) > kMaxIdBytes) return false;
  for (const char* p = text; *p; ++p) {
    if (!asciiAlphaNum(*p) && *p != '-' && *p != '_' && *p != '.') return false;
  }
  return true;
}

bool isValidFamily(const char* text) {
  if (text == nullptr || *text == '\0' || std::strlen(text) > kMaxFamilyBytes) return false;
  if (std::strstr(text, "..") != nullptr) return false;
  for (const char* p = text; *p; ++p) {
    if (!asciiAlphaNum(*p) && *p != '-' && *p != '_') return false;
  }
  return true;
}

bool isValidFileName(const char* text) {
  if (text == nullptr || *text == '\0' || std::strlen(text) > kMaxFileBytes) return false;
  const std::string_view name(text);
  if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
  if (name.find("..") != std::string_view::npos) return false;
  if (!endsWithIgnoreCase(name, ".ttf") && !endsWithIgnoreCase(name, ".otf") &&
      !endsWithIgnoreCase(name, ".ttc")) {
    return false;
  }
  for (const char c : name) {
    if (!asciiAlphaNum(c) && c != '.' && c != '_' && c != '-') return false;
  }
  return name.size() > 4;  // at least one character before ".ttf"
}

bool isValidUrl(const char* text) {
  if (text == nullptr || *text == '\0' || std::strlen(text) > kMaxUrlBytes) return false;
  const std::string_view url(text);
  if (url.rfind("https://", 0) != 0) return false;
  if (url.size() <= 8 || url[8] == '/') return false;
  return !hasControlOrSpace(url);
}

bool parseKind(const char* text, Kind& kind) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "fallback") == 0) {
    kind = Kind::Fallback;
    return true;
  }
  if (std::strcmp(text, "emoji") == 0) {
    kind = Kind::Emoji;
    return true;
  }
  return false;
}

const char* kindName(const Kind kind) {
  switch (kind) {
    case Kind::Fallback:
      return "fallback";
    case Kind::Emoji:
      return "emoji";
  }
  return "fallback";
}

bool parseManifest(const char* data, const size_t len, Pack* out, const size_t capacity, size_t& count,
                   char* error, const size_t errorCap) {
  count = 0;
  if (data == nullptr || len == 0) {
    setError(error, errorCap, 0, "empty manifest");
    return false;
  }
  if (len > kMaxManifestBytes) {
    setError(error, errorCap, 0, "manifest too large");
    return false;
  }
  if (out == nullptr || capacity == 0) {
    setError(error, errorCap, 0, "no output capacity");
    return false;
  }

  bool sawHeader = false;
  size_t lineNumber = 0;
  size_t cursor = 0;
  while (cursor < len) {
    const size_t lineStart = cursor;
    while (cursor < len && data[cursor] != '\n') ++cursor;
    size_t lineEnd = cursor;
    if (cursor < len) ++cursor;  // consume '\n'
    ++lineNumber;
    if (lineEnd > lineStart && data[lineEnd - 1] == '\r') --lineEnd;
    const size_t lineLength = lineEnd - lineStart;
    if (lineLength == 0) continue;
    if (lineLength > kMaxLineBytes) {
      setError(error, errorCap, lineNumber, "line too long");
      return false;
    }
    const std::string_view line(data + lineStart, lineLength);
    if (line[0] == '#') continue;
    if (!sawHeader) {
      if (line != "FEXT1") {
        setError(error, errorCap, lineNumber, "missing FEXT1 header");
        return false;
      }
      sawHeader = true;
      continue;
    }

    const auto fields = splitTabs(line);
    if (fields.size() != kPackFields || fields[0] != "pack") {
      setError(error, errorCap, lineNumber, "expected 9 tab-separated pack fields");
      return false;
    }
    if (count >= capacity || count >= kMaxPacks) {
      setError(error, errorCap, lineNumber, "too many packs");
      return false;
    }

    Pack pack;
    if (!copyField(pack.id, sizeof(pack.id), fields[1], kMaxIdBytes) || !isValidId(pack.id)) {
      setError(error, errorCap, lineNumber, "invalid id");
      return false;
    }
    const std::string kindText(fields[2]);
    if (!parseKind(kindText.c_str(), pack.kind)) {
      setError(error, errorCap, lineNumber, "invalid kind");
      return false;
    }
    if (!copyField(pack.family, sizeof(pack.family), fields[3], kMaxFamilyBytes)) {
      setError(error, errorCap, lineNumber, "invalid family");
      return false;
    }
    if (pack.kind == Kind::Fallback) {
      if (!isValidFamily(pack.family)) {
        setError(error, errorCap, lineNumber, "invalid family");
        return false;
      }
    } else if (std::strcmp(pack.family, "-") != 0) {
      setError(error, errorCap, lineNumber, "emoji pack family must be '-'");
      return false;
    }
    if (fields[4].empty() || fields[4].size() > kMaxNameBytes || hasControl(fields[4])) {
      setError(error, errorCap, lineNumber, "invalid name");
      return false;
    }
    std::memcpy(pack.name, fields[4].data(), fields[4].size());
    pack.name[fields[4].size()] = '\0';

    if (!copyField(pack.file, sizeof(pack.file), fields[5], kMaxFileBytes) || !isValidFileName(pack.file)) {
      setError(error, errorCap, lineNumber, "invalid file");
      return false;
    }
    if (!parseSize(fields[6], pack.size)) {
      setError(error, errorCap, lineNumber, "invalid size");
      return false;
    }
    const std::string sha(fields[7]);
    if (!parseSha256Hex(sha.c_str(), pack.sha256)) {
      setError(error, errorCap, lineNumber, "invalid sha256");
      return false;
    }
    if (!copyField(pack.url, sizeof(pack.url), fields[8], kMaxUrlBytes) || !isValidUrl(pack.url)) {
      setError(error, errorCap, lineNumber, "invalid url");
      return false;
    }

    for (size_t i = 0; i < count; ++i) {
      if (std::strcmp(out[i].id, pack.id) == 0) {
        setError(error, errorCap, lineNumber, "duplicate id");
        return false;
      }
      if (pack.kind == Kind::Fallback && std::strcmp(out[i].family, pack.family) == 0) {
        setError(error, errorCap, lineNumber, "duplicate family");
        return false;
      }
    }
    out[count++] = pack;
  }

  if (!sawHeader) {
    setError(error, errorCap, 0, "missing FEXT1 header");
    return false;
  }
  if (count == 0) {
    setError(error, errorCap, 0, "no packs");
    return false;
  }
  return true;
}

}  // namespace font_extras
