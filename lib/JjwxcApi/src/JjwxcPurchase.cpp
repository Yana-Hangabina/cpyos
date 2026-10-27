#include "JjwxcPurchase.h"

#include <stdio.h>
#include <stdlib.h>
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

// Visible text around an occurrence, tags dropped, bounded. Used for the
// service's own failure sentence so the log and the UI can say what it said.
void sentenceAround(const char* html, const size_t len, const char* at, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  const size_t pos = static_cast<size_t>(at - html);
  size_t start = pos > 80 ? pos - 80 : 0;
  size_t end = pos + 160 < len ? pos + 160 : len;
  size_t used = 0;
  bool lastSpace = true;
  for (size_t i = start; i < end && used + 4 < cap; ++i) {
    const char c = html[i];
    if (c == '<') {
      i = tagClose(html, end, i);
      lastSpace = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!lastSpace) out[used++] = ' ';
      lastSpace = true;
      continue;
    }
    out[used++] = c;
    lastSpace = false;
  }
  out[used] = '\0';
}

bool appendField(char* out, const size_t cap, size_t& used, const char* name, const char* value) {
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

bool parseChapterPrice(const char* html, size_t len, const char* chapterId, ChapterPrice& out) {
  out = ChapterPrice{};
  if (html == nullptr || chapterId == nullptr || len == 0) return false;
  size_t i = 0;
  bool sawAnything = false;
  while (i < len) {
    const char* p = ciFind(html + i, len - i, "<input");
    if (p == nullptr) break;
    const size_t at = static_cast<size_t>(p - html);
    const size_t end = tagClose(html, len, at);
    char name[24];
    char value[16];
    if (end > at && attrValue(html + at, end - at, "name", name, sizeof(name)) &&
        attrValue(html + at, end - at, "value", value, sizeof(value)) && strcmp(value, chapterId) == 0) {
      if (strcmp(name, "chapterid[]") == 0) {
        char point[16];
        out.found = true;
        out.points = attrValue(html + at, end - at, "data-point", point, sizeof(point))
                         ? static_cast<uint32_t>(strtoul(point, nullptr, 10))
                         : 0;
        sawAnything = true;
      } else if (strcmp(name, "buychapterid[]") == 0) {
        out.purchased = true;
        char point[16];
        out.points = attrValue(html + at, end - at, "data-point", point, sizeof(point))
                         ? static_cast<uint32_t>(strtoul(point, nullptr, 10))
                         : out.points;
        sawAnything = true;
      }
    }
    i = end > at ? end : at + 6;
  }
  return sawAnything;
}

PurchaseResult parsePurchaseResult(const char* html, size_t len) {
  PurchaseResult result;
  if (html == nullptr || len == 0) return result;
  const char* ok = ciFind(html, len, "\xE8\xB4\xAD\xE4\xB9\xB0\xE6\x88\x90\xE5\x8A\x9F");  // 购买成功
  if (ok != nullptr) {
    result.outcome = PurchaseOutcome::Success;
    const char* points = ciFind(html, len, "\xE6\x99\x8B\xE6\xB1\x9F\xE5\xB8\x81");  // 晋江币
    if (points != nullptr) {
      const char* p = points + 9;
      while (*p != '\0' && (*p < '0' || *p > '9')) ++p;
      while (*p >= '0' && *p <= '9') result.points = result.points * 10 + (*p++ - '0');
    }
    sentenceAround(html, len, ok, result.message, sizeof(result.message));
    return result;
  }
  const char* wrong = ciFind(html, len, "\xE5\xAF\x86\xE7\xA0\x81\xE9\x94\x99\xE8\xAF\xAF");  // 密码错误
  if (wrong != nullptr) {
    result.outcome = PurchaseOutcome::WrongPassword;
    sentenceAround(html, len, wrong, result.message, sizeof(result.message));
    return result;
  }
  const char* poor = ciFind(html, len, "\xE4\xBD\x99\xE9\xA2\x9D\xE4\xB8\x8D\xE8\xB6\xB3");  // 余额不足
  if (poor != nullptr) {
    result.outcome = PurchaseOutcome::Insufficient;
    sentenceAround(html, len, poor, result.message, sizeof(result.message));
    return result;
  }
  const char* owned = ciFind(html, len, "\xE5\xB7\xB2\xE8\xB4\xAD\xE4\xB9\xB0");  // 已购买
  if (owned != nullptr) {
    result.outcome = PurchaseOutcome::AlreadyOwned;
    sentenceAround(html, len, owned, result.message, sizeof(result.message));
    return result;
  }
  // No sentence we recognise. Keep the page's title if it has one, so a photo of
  // the screen plus this line can identify a new failure shape.
  const char* title = ciFind(html, len, "<title>");
  if (title != nullptr) {
    sentenceAround(html, len, title + 7, result.message, sizeof(result.message));
  }
  return result;
}

size_t buildBuyBody(const char* novelId, const char* chapterId, const char* password, char* out, size_t cap) {
  if (novelId == nullptr || chapterId == nullptr || password == nullptr || out == nullptr || cap == 0) return 0;
  if (strlen(password) != 6) return 0;
  for (size_t i = 0; i < 6; ++i) {
    if (password[i] < '0' || password[i] > '9') return 0;
  }
  size_t used = 0;
  if (!appendField(out, cap, used, "novel_id", novelId)) return 0;
  if (!appendField(out, cap, used, "chapterid[]", chapterId)) return 0;
  if (!appendField(out, cap, used, "renew_status", "0")) return 0;
  if (!appendField(out, cap, used, "buytype", "chapters")) return 0;
  if (!appendField(out, cap, used, "not_use_backpoint", "0")) return 0;
  if (!appendField(out, cap, used, "checkps", "0")) return 0;
  if (!appendField(out, cap, used, "shumeideviceId", "get_deviceid_timeout")) return 0;
  if (!appendField(out, cap, used, "password", password)) return 0;
  if (!appendField(out, cap, used, "but", "\xE7\xA1\xAE\xE5\xAE\x9E\xE8\xAE\xA2\xE8\xB4\xAD")) return 0;  // 确实订购
  return used;
}

}  // namespace jjwxc
