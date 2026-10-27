#include "JjwxcDeviceAuth.h"

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

// Reads the value of `key` as a string or bare token. Returns false when absent.
bool valueOf(const char* body, const size_t len, const char* key, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  const char* p = ciFind(body, len, key);
  if (p == nullptr) return false;
  p += strlen(key);
  const char* end = body + len;
  while (p < end && *p != ':') ++p;
  if (p >= end) return false;
  ++p;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  if (p >= end) return false;
  if (*p == '"') {
    ++p;
    size_t used = 0;
    while (p < end && *p != '"' && used + 1 < cap) {
      if (*p == '\\' && p + 1 < end) ++p;
      out[used++] = *p++;
    }
    out[used] = '\0';
    return used > 0;
  }
  const char* start = p;
  while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
  size_t n = static_cast<size_t>(p - start);
  if (n >= cap) n = cap - 1;
  memcpy(out, start, n);
  out[n] = '\0';
  return n > 0;
}

bool codeIs200(const char* body, const size_t len) {
  char code[16];
  return valueOf(body, len, "\"code\"", code, sizeof(code)) && strcmp(code, "200") == 0;
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

bool parseAuthKey(const char* body, size_t len, char* out, size_t cap) {
  if (body == nullptr || len == 0 || !codeIs200(body, len)) return false;
  return valueOf(body, len, "\"authKey\"", out, cap);
}

bool parseApproval(const char* body, size_t len) {
  return body != nullptr && len > 0 && codeIs200(body, len);
}

size_t buildNotifyBody(const char* loginName, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "loginName", loginName)) return 0;
  return used;
}

size_t buildAskBody(const char* authKey, char* out, size_t cap) {
  size_t used = 0;
  if (!appendField(out, cap, used, "authKey", authKey)) return 0;
  if (!appendField(out, cap, used, "action", "check")) return 0;
  return used;
}

}  // namespace jjwxc
