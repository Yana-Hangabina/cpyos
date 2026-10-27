#include "JjwxcFavorites.h"

#include <stdio.h>
#include <string.h>

namespace jjwxc {
namespace {

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool ciMatch(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n > len) return false;
  for (size_t i = 0; i < n; ++i) {
    if (asciiLower(text[i]) != asciiLower(lower[i])) return false;
  }
  return true;
}

const char* ciFind(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n == 0 || n > len) return nullptr;
  for (size_t i = 0; i + n <= len; ++i) {
    if (ciMatch(text + i, len - i, lower)) return text + i;
  }
  return nullptr;
}

size_t tagClose(const char* html, const size_t len, const size_t from) {
  char quote = 0;
  for (size_t i = from + 1; i < len; ++i) {
    const char c = html[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i + 1;
    }
  }
  return len;
}

bool attrValue(const char* tag, const size_t tagLen, const char* lowerName, char* out, size_t outCap) {
  if (out == nullptr || outCap == 0) return false;
  out[0] = '\0';
  const size_t nameLen = strlen(lowerName);
  for (size_t i = 1; i + nameLen < tagLen; ++i) {
    const char prev = tag[i - 1];
    if (!(prev == '<' || prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n' || prev == '/')) continue;
    if (!ciMatch(tag + i, tagLen - i, lowerName)) continue;
    size_t j = i + nameLen;
    while (j < tagLen && (tag[j] == ' ' || tag[j] == '\t' || tag[j] == '\r' || tag[j] == '\n')) ++j;
    if (j >= tagLen || tag[j] != '=') continue;
    ++j;
    while (j < tagLen && (tag[j] == ' ' || tag[j] == '\t' || tag[j] == '\r' || tag[j] == '\n')) ++j;
    char quote = 0;
    if (j < tagLen && (tag[j] == '"' || tag[j] == '\'')) {
      quote = tag[j];
      ++j;
    }
    const size_t begin = j;
    if (quote != 0) {
      while (j < tagLen && tag[j] != quote) ++j;
    } else {
      while (j < tagLen && tag[j] != '>' && tag[j] != ' ' && tag[j] != '\t') ++j;
    }
    const size_t n = j - begin;
    if (n + 1 > outCap) return false;
    memcpy(out, tag + begin, n);
    out[n] = '\0';
    return true;
  }
  return false;
}

// Visible text, tags dropped, a few entities decoded, whitespace collapsed.
void extractText(const char* html, const size_t len, char* out, const size_t cap) {
  size_t used = 0;
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  bool lastSpace = true;
  for (size_t i = 0; i < len;) {
    const char c = html[i];
    if (c == '<') {
      i = tagClose(html, len, i);
      lastSpace = true;
      continue;
    }
    if (c == '&' && i + 1 < len) {
      const char* semi = static_cast<const char*>(memchr(html + i, ';', len - i));
      if (semi != nullptr && semi - (html + i) <= 8) {
        const size_t n = static_cast<size_t>(semi - (html + i) + 1);
        const char* ent = html + i + 1;
        const size_t el = n - 2;
        const char* rep = nullptr;
        char one[2] = {};
        if (el == 3 && ciMatch(ent, el, "amp")) {
          rep = "&";
        } else if (el == 2 && ciMatch(ent, el, "lt")) {
          rep = "<";
        } else if (el == 2 && ciMatch(ent, el, "gt")) {
          rep = ">";
        } else if (el == 4 && ciMatch(ent, el, "quot")) {
          rep = "\"";
        } else if (el == 4 && ciMatch(ent, el, "nbsp")) {
          rep = " ";
        } else if (el >= 2 && ent[0] == '#') {
          unsigned cp = 0;
          size_t k = 1;
          if (ent[1] == 'x' || ent[1] == 'X') {
            k = 2;
            for (; k < el; ++k) {
              const char d = asciiLower(ent[k]);
              if (d >= '0' && d <= '9') cp = cp * 16 + static_cast<unsigned>(d - '0');
              else if (d >= 'a' && d <= 'f') cp = cp * 16 + static_cast<unsigned>(d - 'a' + 10);
              else { cp = 0; break; }
            }
          } else {
            for (; k < el; ++k) {
              if (ent[k] < '0' || ent[k] > '9') { cp = 0; break; }
              cp = cp * 10 + static_cast<unsigned>(ent[k] - '0');
            }
          }
          if (cp != 0 && cp <= 0x10FFFF) {
            // Only the common ranges matter here; the folder names are Chinese.
            if (cp < 0x80) {
              one[0] = static_cast<char>(cp);
              one[1] = '\0';
              rep = one;
            } else if (cp < 0x800) {
              const char pair[3] = {static_cast<char>(0xC0 | (cp >> 6)),
                                    static_cast<char>(0x80 | (cp & 0x3F)), 0};
              if (used + 2 < cap) {
                memcpy(out + used, pair, 2);
                used += 2;
                out[used] = '\0';
              }
              i += n;
              lastSpace = false;
              continue;
            } else {
              const char three[4] = {static_cast<char>(0xE0 | (cp >> 12)),
                                     static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                                     static_cast<char>(0x80 | (cp & 0x3F)), 0};
              if (used + 3 < cap) {
                memcpy(out + used, three, 3);
                used += 3;
                out[used] = '\0';
              }
              i += n;
              lastSpace = false;
              continue;
            }
          }
        }
        if (rep != nullptr) {
          const size_t rl = strlen(rep);
          if (used + rl < cap) {
            memcpy(out + used, rep, rl);
            used += rl;
            out[used] = '\0';
          }
          i += n;
          lastSpace = false;
          continue;
        }
      }
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!lastSpace && used + 1 < cap) {
        out[used++] = ' ';
        out[used] = '\0';
      }
      lastSpace = true;
      ++i;
      continue;
    }
    const unsigned char uc = static_cast<unsigned char>(c);
    size_t seq = 1;
    if ((uc & 0xE0) == 0xC0) seq = 2;
    else if ((uc & 0xF0) == 0xE0) seq = 3;
    else if ((uc & 0xF8) == 0xF0) seq = 4;
    if (i + seq > len) seq = 1;
    if (used + seq < cap) {
      memcpy(out + used, html + i, seq);
      used += seq;
      out[used] = '\0';
    }
    lastSpace = false;
    i += seq;
  }
  while (used > 0 && out[used - 1] == ' ') out[--used] = '\0';
}

bool appendField(char* out, const size_t cap, size_t& used, const char* name, const char* value) {
  if (out == nullptr || cap == 0) return false;
  if (used > 0) {
    if (used + 1 >= cap) return false;
    out[used++] = '&';
  }
  const char* parts[2] = {name, value};
  for (int p = 0; p < 2; ++p) {
    if (p == 1) {
      if (used + 1 >= cap) return false;
      out[used++] = '=';
    }
    const char* s = parts[p] != nullptr ? parts[p] : "";
    for (size_t i = 0; s[i] != '\0'; ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                              c == '.' || c == '_' || c == '~';
      if (unreserved) {
        if (used + 1 >= cap) return false;
        out[used++] = static_cast<char>(c);
      } else if (c == ' ') {
        if (used + 1 >= cap) return false;
        out[used++] = '+';
      } else {
        if (used + 3 >= cap) return false;
        static const char kHex[] = "0123456789ABCDEF";
        out[used++] = '%';
        out[used++] = kHex[c >> 4];
        out[used++] = kHex[c & 0xF];
      }
    }
  }
  out[used] = '\0';
  return true;
}

}  // namespace

size_t parseFavoriteClasses(const char* html, size_t len, FavoriteClassSink sink, void* ctx) {
  if (html == nullptr || sink == nullptr || len == 0) return 0;
  size_t count = 0;
  size_t i = 0;
  while (i < len) {
    const char* p = ciFind(html + i, len - i, "<div");
    if (p == nullptr) break;
    const size_t at = static_cast<size_t>(p - html);
    const size_t openEnd = tagClose(html, len, at);
    char klass[128];
    char id[16];
    if (openEnd > at && attrValue(html + at, openEnd - at, "class", klass, sizeof(klass)) &&
        ciFind(klass, strlen(klass), "one_class") != nullptr &&
        attrValue(html + at, openEnd - at, "data-classid", id, sizeof(id))) {
      // The class name is the immediate child div; the first one within a short
      // window is always it (the markup nests nothing before it).
      const char* nameDiv = ciFind(html + openEnd, len - openEnd, "class=\"class_name");
      if (nameDiv != nullptr && static_cast<size_t>(nameDiv - (html + openEnd)) < 300) {
        const size_t nameAt = static_cast<size_t>(nameDiv - html);
        const size_t nameTagEnd = tagClose(html, len, nameAt);
        const char* close = ciFind(html + nameTagEnd, len - nameTagEnd, "</div");
        if (nameTagEnd > nameAt && close != nullptr) {
          FavoriteClass item;
          snprintf(item.id, sizeof(item.id), "%s", id);
          extractText(html + nameTagEnd, static_cast<size_t>(close - (html + nameTagEnd)), item.name,
                      sizeof(item.name));
          if (item.id[0] != '\0' && item.name[0] != '\0') {
            if (!sink(ctx, item, count)) return count;
            ++count;
          }
        }
      }
    }
    i = openEnd > at ? openEnd : at + 4;
  }
  return count;
}

bool parseFavoriteResult(const char* body, size_t len, FavoriteResult& out) {
  out = FavoriteResult{};
  if (body == nullptr || len == 0) return false;
  const char* codeKey = ciFind(body, len, "\"code\"");
  if (codeKey != nullptr) {
    const char* p = codeKey + 6;
    const char* end = body + len;
    while (p < end && *p != ':') ++p;
    if (p < end) {
      ++p;
      while (p < end && (*p == ' ' || *p == '"')) ++p;
      int value = 0;
      bool digits = false;
      while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        digits = true;
        ++p;
      }
      if (digits) {
        out.code = value;
        out.ok = (value == 200 || value == 0);
      }
    }
  }
  const char* msgKey = ciFind(body, len, "\"message\"");
  if (msgKey != nullptr) {
    const char* p = msgKey + 9;
    const char* end = body + len;
    while (p < end && *p != ':') ++p;
    if (p < end) {
      ++p;
      while (p < end && *p != '"') ++p;
      if (p < end) {
        ++p;
        size_t used = 0;
        while (p < end && *p != '"' && used + 1 < sizeof(out.message)) {
          if (*p == '\\' && p + 1 < end) {
            ++p;
            if (*p == 'n') out.message[used++] = ' ';
            else out.message[used++] = *p;
          } else {
            out.message[used++] = *p;
          }
          ++p;
        }
        out.message[used] = '\0';
      }
    }
  }
  if (out.code < 0) {
    // A couple of these endpoints answer a plain sentence instead of JSON. Only a
    // literal success sentence is accepted, never a bare HTTP 200.
    if (ciFind(body, len, "\xE6\x88\x90\xE5\x8A\x9F") != nullptr || ciFind(body, len, "success") != nullptr) {
      out.ok = true;
      out.code = 200;
      snprintf(out.message, sizeof(out.message), "%s", "success");
    }
  }
  return out.code >= 0;
}

bool parseFavoriteStatus(const char* html, size_t len, FavoriteStatus& out) {
  out = FavoriteStatus{};
  if (html == nullptr || len == 0) return false;
  if (ciFind(html, len, "addnovelfavclass") != nullptr) {
    out.known = true;
    out.isFavorite = false;
    return true;
  }
  // "已成功收藏文章" in UTF-8; editNovelFavClass is the change-class handler.
  if (ciFind(html, len, "editnovelfavclass") != nullptr ||
      ciFind(html, len, "\xE5\xB7\xB2\xE6\x88\x90\xE5\x8A\x9F\xE6\x94\xB6\xE8\x97\x8F") != nullptr) {
    out.known = true;
    out.isFavorite = true;
    return true;
  }
  return false;
}

bool parseFavoriteStatusResponse(const char* body, size_t len, FavoriteStatusInfo& out) {
  out = FavoriteStatusInfo{};
  if (body == nullptr || len == 0) return false;
  // code may be numeric or quoted; require 200.
  const char* codeKey = ciFind(body, len, "\"code\"");
  if (codeKey == nullptr) return false;
  const char* p = codeKey + 6;
  const char* end = body + len;
  while (p < end && *p != ':') ++p;
  if (p >= end) return false;
  ++p;
  while (p < end && (*p == ' ' || *p == '"')) ++p;
  if (!(p + 2 < end && p[0] == '2' && p[1] == '0' && p[2] == '0')) return false;

  const char* statusKey = ciFind(body, len, "\"status\"");
  if (statusKey == nullptr) return false;
  p = statusKey + 8;
  while (p < end && *p != ':') ++p;
  if (p >= end) return false;
  ++p;
  while (p < end && (*p == ' ' || *p == '"')) ++p;
  if (p >= end) return false;
  out.isFavorite = (*p == '1');
  out.known = (*p == '0' || *p == '1');

  auto readField = [&](const char* key, char* dst, const size_t cap) {
    const char* at = ciFind(body, len, key);
    if (at == nullptr) return;
    at += strlen(key);
    while (at < end && *at != ':') ++at;
    if (at >= end) return;
    ++at;
    while (at < end && (*at == ' ' || *at == '"')) ++at;
    size_t used = 0;
    while (at < end && *at != '"' && *at != ',' && *at != '}' && used + 1 < cap) {
      if (*at == '\\' && at + 1 < end) ++at;
      dst[used++] = *at++;
    }
    dst[used] = '\0';
  };
  readField("\"classid\"", out.classId, sizeof(out.classId));
  readField("\"class_name\"", out.className, sizeof(out.className));
  return out.known;
}

size_t buildFavoriteStatusBody(const char* novelId, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "favnovelid", novelId)) return 0;
  return used;
}

size_t buildFavoriteAddBody(const char* novelId, const char* classId, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "novelidstr", novelId)) return 0;
  if (!appendField(out, cap, used, "chapterid", "0")) return 0;
  if (!appendField(out, cap, used, "classid", classId)) return 0;
  return used;
}

size_t buildFavoriteMoveBody(const char* novelIds, const char* classId, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "novelids", novelIds)) return 0;
  if (!appendField(out, cap, used, "chapterid", "0")) return 0;
  if (!appendField(out, cap, used, "classid", classId)) return 0;
  return used;
}

size_t buildFavoriteDeleteBody(const char* novelIds, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "novelids", novelIds)) return 0;
  if (!appendField(out, cap, used, "isread", "0")) return 0;
  return used;
}

size_t buildFavoriteClassCreateBody(const char* name, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "sort", "0")) return 0;
  if (!appendField(out, cap, used, "classname", name)) return 0;
  return used;
}

size_t buildFavoriteClassRenameBody(const char* classId, const char* name, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "newclassname", name)) return 0;
  if (!appendField(out, cap, used, "sort", "0")) return 0;
  if (!appendField(out, cap, used, "classid", classId)) return 0;
  return used;
}

size_t buildFavoriteClassDeleteBody(const char* classId, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "classid", classId)) return 0;
  return used;
}

}  // namespace jjwxc
