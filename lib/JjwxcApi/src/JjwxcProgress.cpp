#include "JjwxcProgress.h"

#include <stdio.h>
#include <string.h>

#include "JjwxcCrypto.h"

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

// Moves past a JSON key and its colon.
const char* afterKey(const char* body, const size_t len, const char* key) {
  const char* p = ciFind(body, len, key);
  if (p == nullptr) return nullptr;
  const char* end = body + len;
  p += strlen(key);
  while (p < end && *p != ':') ++p;
  if (p >= end) return nullptr;
  ++p;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  return p;
}

void copyText(char* out, const size_t cap, const char* s, size_t n) {
  if (out == nullptr || cap == 0) return;
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

// Reads a quoted JSON string, unescaping the few sequences the service can put in
// a chapter snippet. Stops at the closing quote or a buffer boundary.
bool readJsonString(const char* p, const char* end, char* out, const size_t cap) {
  if (p == nullptr || p >= end || *p != '"') return false;
  ++p;
  size_t used = 0;
  while (p < end && *p != '"') {
    char c = *p;
    if (c == '\\' && p + 1 < end) {
      ++p;
      c = *p;
      if (c == 'n' || c == 'r' || c == 't') c = ' ';
    }
    if (used + 1 < cap) out[used++] = c;
    ++p;
  }
  if (out != nullptr && cap > 0) out[used] = '\0';
  return used > 0;
}

bool jsonString(const char* body, const size_t len, const char* key, char* out, const size_t cap) {
  if (out != nullptr && cap > 0) out[0] = '\0';
  const char* p = afterKey(body, len, key);
  if (p == nullptr) return false;
  if (*p == 'n') return false;  // null
  if (*p != '"') {
    // Some numeric fields arrive quoted and some do not; accept both.
    const char* end = body + len;
    const char* start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
    copyText(out, cap, start, static_cast<size_t>(p - start));
    return out != nullptr && out[0] != '\0';
  }
  return readJsonString(p, body + len, out, cap);
}

uint32_t jsonUint(const char* body, const size_t len, const char* key) {
  const char* p = afterKey(body, len, key);
  if (p == nullptr) return 0;
  if (*p == '"') ++p;
  uint32_t value = 0;
  bool digits = false;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + static_cast<uint32_t>(*p - '0');
    digits = true;
    ++p;
  }
  return digits ? value : 0;
}

bool codeIsSuccess(const char* body, const size_t len) {
  char code[16];
  if (jsonString(body, len, "\"code\"", code, sizeof(code))) {
    return strcmp(code, "200") == 0 || strcmp(code, "0") == 0;
  }
  return false;
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

// Appends JSON-escaped text, truncating on a UTF-8 boundary.
void appendEscapedJson(char* out, const size_t cap, size_t& used, const char* s) {
  if (s == nullptr) return;
  for (size_t i = 0; s[i] != '\0' && used + 8 < cap; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\') {
      if (used + 2 >= cap) break;
      out[used++] = '\\';
      out[used++] = static_cast<char>(c);
    } else if (c == '\n' || c == '\r' || c == '\t') {
      out[used++] = ' ';
    } else if (c < 0x20) {
      out[used++] = ' ';
    } else {
      // Copy a whole UTF-8 sequence; never leave a partial one.
      size_t seq = 1;
      if ((c & 0xE0) == 0xC0) seq = 2;
      else if ((c & 0xF0) == 0xE0) seq = 3;
      else if ((c & 0xF8) == 0xF0) seq = 4;
      if (used + seq >= cap) break;
      memcpy(out + used, s + i, seq);
      used += seq;
      i += seq - 1;
    }
  }
  out[used] = '\0';
}

}  // namespace

bool parseReadingProgress(const char* body, size_t len, ReadingProgress& out) {
  out = ReadingProgress{};
  if (body == nullptr || len == 0) return false;
  if (!codeIsSuccess(body, len)) return false;
  // A successful answer with an empty data object is the normal "local and cloud
  // agree" case, not a malformed document.
  if (!jsonString(body, len, "\"chapterid\"", out.chapterId, sizeof(out.chapterId))) return true;
  out.valid = out.chapterId[0] != '\0';
  jsonString(body, len, "\"text\"", out.text, sizeof(out.text));
  out.position = jsonUint(body, len, "\"position\"");
  jsonString(body, len, "\"source\"", out.source, sizeof(out.source));
  jsonString(body, len, "\"readingtime\"", out.readingTime, sizeof(out.readingTime));
  jsonString(body, len, "\"readpercentage\"", out.readPercentage, sizeof(out.readPercentage));
  return true;
}

bool readingProgressResponseOk(const char* body, size_t len) {
  return body != nullptr && len > 0 && codeIsSuccess(body, len);
}

bool hasReadingProgress(const char* body, size_t len) {
  if (body == nullptr || len == 0) return false;
  if (!codeIsSuccess(body, len)) return false;
  char chapter[16];
  return jsonString(body, len, "\"chapterid\"", chapter, sizeof(chapter)) && chapter[0] != '\0';
}

size_t buildProgressReadBody(const char* novelId, const char* chapterId, const char* sid, char* out, size_t cap) {
  if (novelId == nullptr || chapterId == nullptr || sid == nullptr || out == nullptr || cap == 0) return 0;
  char token[512];
  const size_t tokenLen = Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), token, sizeof(token));
  if (tokenLen == 0) return 0;
  size_t used = 0;
  if (!appendField(out, cap, used, "novelid", novelId)) return 0;
  if (!appendField(out, cap, used, "chapterid", chapterId)) return 0;
  if (!appendField(out, cap, used, "source", "wap")) return 0;
  if (!appendField(out, cap, used, "token", token)) return 0;
  return used;
}

size_t buildProgressWriteBody(const char* novelId, const char* readerId, const char* chapterId, uint32_t position,
                              const char* text, uint32_t readingTime, const char* sid, char* out, size_t cap) {
  if (novelId == nullptr || readerId == nullptr || chapterId == nullptr || sid == nullptr || out == nullptr ||
      cap == 0) {
    return 0;
  }
  char json[768];
  size_t used = 0;
  const int head = snprintf(json, sizeof(json),
                            "[{\"readerid\":\"%s\",\"novelid\":\"%s\",\"chapterid\":\"%s\",\"position\":%lu,"
                            "\"text\":\"",
                            readerId, novelId, chapterId, static_cast<unsigned long>(position));
  if (head <= 0 || static_cast<size_t>(head) >= sizeof(json)) return 0;
  used = static_cast<size_t>(head);
  appendEscapedJson(json, sizeof(json), used, text != nullptr ? text : "");
  const int tail = snprintf(json + used, sizeof(json) - used,
                            "\",\"readingtime\":%lu,\"source\":\"wap\",\"versioncode\":381,"
                            "\"phonemodel\":\"crosspoint\",\"dateline\":%lu}]",
                            static_cast<unsigned long>(readingTime), static_cast<unsigned long>(readingTime));
  if (tail <= 0 || static_cast<size_t>(tail) >= sizeof(json) - used) return 0;

  char token[512];
  if (Base64::encode(reinterpret_cast<const uint8_t*>(sid), strlen(sid), token, sizeof(token)) == 0) return 0;

  size_t outUsed = 0;
  if (!appendField(out, cap, outUsed, "readingprogressstring", json)) return 0;
  if (!appendField(out, cap, outUsed, "user_token", token)) return 0;
  return outUsed;
}

bool readerIdFromSid(const char* sid, char* out, size_t cap) {
  if (sid == nullptr || out == nullptr || cap < 10) return false;
  for (int i = 0; i < 8; ++i) {
    if (sid[i] < '0' || sid[i] > '9') return false;
  }
  if (sid[8] != '_') return false;
  memcpy(out, sid, 8);
  out[8] = '\0';
  return true;
}

}  // namespace jjwxc
