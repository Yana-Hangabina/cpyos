#include "HtmlScanner.h"

#include <cctype>
#include <cstring>

namespace webbrowser {
namespace {

constexpr size_t kNpos = std::string_view::npos;

struct NamedEntity {
  const char* name;
  const char* utf8;
};

// The subset that appears in Chinese/English reading pages. Numeric references
// cover everything else; the full HTML5 table is ~2,200 entries and is not worth
// the flash here.
constexpr NamedEntity kEntities[] = {
    {"amp", "&"},          {"lt", "<"},           {"gt", ">"},
    {"quot", "\""},        {"apos", "'"},         {"nbsp", "\xC2\xA0"},
    {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"},
    {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"lsquo", "\xE2\x80\x98"},
    {"rsquo", "\xE2\x80\x99"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
    {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"}, {"times", "\xC3\x97"},
    {"divide", "\xC3\xB7"}, {"copy", "\xC2\xA9"},    {"reg", "\xC2\xAE"},
    {"deg", "\xC2\xB0"},   {"plusmn", "\xC2\xB1"}, {"frac12", "\xC2\xBD"},
    {"sup2", "\xC2\xB2"},  {"yen", "\xC2\xA5"},    {"euro", "\xE2\x82\xAC"},
    {"pound", "\xC2\xA3"}, {"sect", "\xC2\xA7"},   {"para", "\xC2\xB6"},
    {"ensp", "\xE2\x80\x82"}, {"emsp", "\xE2\x80\x83"}, {"thinsp", "\xE2\x80\x89"},
    {"zwnj", "\xE2\x80\x8C"}, {"zwj", "\xE2\x80\x8D"},
    {"larr", "\xE2\x86\x90"}, {"rarr", "\xE2\x86\x92"},
    {"uarr", "\xE2\x86\x91"}, {"darr", "\xE2\x86\x93"},
};

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out += "\xEF\xBF\xBD";
  }
}

bool decodeEntity(const std::string_view s, size_t& i, std::string& out) {
  const size_t semi = s.find(';', i + 1);
  if (semi == kNpos || semi - i > 32) return false;
  const std::string_view body = s.substr(i + 1, semi - i - 1);
  if (body.empty()) return false;

  if (body[0] == '#') {
    uint32_t cp = 0;
    bool ok = false;
    if (body.size() > 1 && (body[1] == 'x' || body[1] == 'X')) {
      for (size_t k = 2; k < body.size(); ++k) {
        const char c = body[k];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
        else { ok = false; break; }
        cp = cp * 16 + digit;
        ok = true;
        if (cp > 0x10FFFF) break;
      }
    } else {
      for (size_t k = 1; k < body.size(); ++k) {
        const char c = body[k];
        if (c < '0' || c > '9') { ok = false; break; }
        cp = cp * 10 + static_cast<uint32_t>(c - '0');
        ok = true;
        if (cp > 0x10FFFF) break;
      }
    }
    if (!ok || cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    appendUtf8(out, cp);
    i = semi + 1;
    return true;
  }

  for (const auto& e : kEntities) {
    if (body == e.name) {
      out += e.utf8;
      i = semi + 1;
      return true;
    }
  }
  return false;
}

void decodeText(const std::string_view in, std::string& out, const bool decodeEntities) {
  out.clear();
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (decodeEntities && in[i] == '&' && decodeEntity(in, i, out)) continue;
    out.push_back(in[i++]);
  }
}

bool isAlpha(const char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isSpace(const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool isNameChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == ':' || c == '.';
}

size_t findCaseInsensitive(const std::string_view hay, size_t from, const std::string_view needle) {
  if (needle.empty() || needle.size() > hay.size()) return kNpos;
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    bool match = true;
    for (size_t k = 0; k < needle.size(); ++k) {
      if (std::tolower(static_cast<unsigned char>(hay[i + k])) !=
          std::tolower(static_cast<unsigned char>(needle[k]))) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return kNpos;
}

bool isRawTextTag(const std::string_view name) {
  return name == "script" || name == "style" || name == "title" || name == "textarea" || name == "xmp" ||
         name == "plaintext";
}

void lowerInto(std::string& out, const std::string_view in) {
  out.clear();
  out.reserve(in.size());
  for (const char c : in) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
}

}  // namespace

bool HtmlScanner::next(HtmlToken& out) {
  for (;;) {
    out = HtmlToken{};
    if (inRawText() && emitRawText(out)) return true;
    if (pos_ >= html_.size()) return false;

    if (html_[pos_] != '<') {
      return emitText(out);
    }

    const char look = pos_ + 1 < html_.size() ? html_[pos_ + 1] : '\0';
    if (look == '!') {
      skipDeclaration();
      continue;
    }
    if (look == '?') {
      const size_t close = html_.find('>', pos_ + 2);
      pos_ = close == kNpos ? html_.size() : close + 1;
      continue;
    }
    if (look == '/') return parseEndTag(out);
    if (isAlpha(look)) return parseStartTag(out);
    // A bare '<' is text.
    return emitText(out);
  }
}

bool HtmlScanner::emitText(HtmlToken& out) {
  if (pos_ >= html_.size()) return false;
  size_t end = pos_;
  for (;;) {
    end = html_.find('<', end);
    if (end == kNpos) {
      end = html_.size();
      break;
    }
    const char look = end + 1 < html_.size() ? html_[end + 1] : '\0';
    if (isAlpha(look) || look == '/' || look == '!' || look == '?') break;
    ++end;  // the '<' belongs to the text run
  }
  if (end == pos_) return false;
  decodeText(html_.substr(pos_, end - pos_), textBuf_, true);
  pos_ = end;
  out.type = HtmlTokenType::Text;
  out.text = textBuf_;
  return true;
}

bool HtmlScanner::parseStartTag(HtmlToken& out) {
  size_t i = pos_ + 1;
  const size_t nameStart = i;
  while (i < html_.size() && isNameChar(html_[i])) ++i;
  lowerInto(nameBuf_, html_.substr(nameStart, i - nameStart));
  pos_ = i;
  out.type = HtmlTokenType::StartTag;
  out.name = nameBuf_;
  out.attributeCount = 0;
  bool selfClosing = false;

  while (pos_ < html_.size()) {
    while (pos_ < html_.size() && isSpace(html_[pos_])) ++pos_;
    if (pos_ >= html_.size()) break;
    const char c = html_[pos_];
    if (c == '>') {
      ++pos_;
      break;
    }
    if (c == '/') {
      selfClosing = true;
      ++pos_;
      continue;
    }

    const size_t attrStart = pos_;
    while (pos_ < html_.size() && !isSpace(html_[pos_]) && html_[pos_] != '=' && html_[pos_] != '>' &&
           html_[pos_] != '/') {
      ++pos_;
    }
    if (pos_ == attrStart) {
      ++pos_;
      continue;
    }
    std::string attrName;
    lowerInto(attrName, html_.substr(attrStart, pos_ - attrStart));

    while (pos_ < html_.size() && isSpace(html_[pos_])) ++pos_;
    std::string_view rawValue;
    if (pos_ < html_.size() && html_[pos_] == '=') {
      ++pos_;
      while (pos_ < html_.size() && isSpace(html_[pos_])) ++pos_;
      if (pos_ < html_.size() && (html_[pos_] == '"' || html_[pos_] == '\'')) {
        const char quote = html_[pos_++];
        const size_t valueStart = pos_;
        const size_t valueEnd = html_.find(quote, pos_);
        if (valueEnd == kNpos) {
          rawValue = html_.substr(valueStart);
          pos_ = html_.size();
        } else {
          rawValue = html_.substr(valueStart, valueEnd - valueStart);
          pos_ = valueEnd + 1;
        }
      } else {
        const size_t valueStart = pos_;
        while (pos_ < html_.size() && !isSpace(html_[pos_]) && html_[pos_] != '>') ++pos_;
        rawValue = html_.substr(valueStart, pos_ - valueStart);
      }
    }

    if (out.attributeCount < 8) {
      const uint8_t slot = out.attributeCount++;
      attrNames_[slot] = attrName;
      decodeText(rawValue, attrValues_[slot], true);
      out.attributes[slot].name = attrNames_[slot];
      out.attributes[slot].value = attrValues_[slot];
    }
  }

  out.selfClosing = selfClosing;
  if (!selfClosing && isRawTextTag(nameBuf_)) rawTag_ = nameBuf_;
  return true;
}

bool HtmlScanner::parseEndTag(HtmlToken& out) {
  size_t i = pos_ + 2;
  while (i < html_.size() && isSpace(html_[i])) ++i;
  const size_t nameStart = i;
  while (i < html_.size() && isNameChar(html_[i])) ++i;
  lowerInto(nameBuf_, html_.substr(nameStart, i - nameStart));
  const size_t close = html_.find('>', i);
  pos_ = close == kNpos ? html_.size() : close + 1;
  out.type = HtmlTokenType::EndTag;
  out.name = nameBuf_;
  return true;
}

void HtmlScanner::skipDeclaration() {
  if (html_.substr(pos_, 4) == "<!--") {
    const size_t close = html_.find("-->", pos_ + 4);
    pos_ = close == kNpos ? html_.size() : close + 3;
    return;
  }
  const size_t close = html_.find('>', pos_ + 2);
  pos_ = close == kNpos ? html_.size() : close + 1;
}

bool HtmlScanner::emitRawText(HtmlToken& out) {
  const std::string closeTag = "</" + rawTag_;
  const size_t close = findCaseInsensitive(html_, pos_, closeTag);
  std::string tag;
  tag.swap(rawTag_);
  std::string content;
  if (close == kNpos) {
    content.assign(html_.substr(pos_));
    pos_ = html_.size();
  } else {
    content.assign(html_.substr(pos_, close - pos_));
    pos_ = close;
  }
  if (content.empty()) return false;
  const bool decodeEntities = tag == "title" || tag == "textarea";
  decodeText(content, textBuf_, decodeEntities);
  out.type = HtmlTokenType::Text;
  out.text = textBuf_;
  return true;
}

}  // namespace webbrowser
