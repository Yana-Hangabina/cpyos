// Minimal application/x-www-form-urlencoded and JSON-string-array parsing for
// the documented file-management endpoints (/mkdir, /rename, /move, /delete).
// ESP-IDF-free so the rules have host tests.
#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace form_fields {

using Field = std::pair<std::string, std::string>;

inline std::string urlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      const auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      out += static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2]));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

inline void parseForm(const std::string& body, std::vector<Field>& fields) {
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t amp = body.find('&', pos);
    const std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && eq > 0) {
      fields.emplace_back(urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1)));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
}

inline const std::string* get(const std::vector<Field>& fields, const char* name) {
  for (const auto& field : fields) {
    if (field.first == name) return &field.second;
  }
  return nullptr;
}

// Extracts the strings of a JSON array of strings. Deliberately small: the
// documented `paths` field is only ever `["/a","/b"]`, and malformed input is
// rejected instead of guessed at.
inline bool parseJsonStringArray(const std::string& body, std::vector<std::string>& out) {
  size_t i = 0;
  while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
  if (i >= body.size() || body[i] != '[') return false;
  ++i;
  out.clear();
  bool afterComma = false;
  for (;;) {
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i < body.size() && body[i] == ']') return !afterComma;
    if (i >= body.size() || body[i] != '"') return false;
    afterComma = false;
    ++i;
    std::string value;
    while (i < body.size() && body[i] != '"') {
      if (body[i] == '\\') {
        if (++i >= body.size()) return false;
        value += body[i];
      } else {
        value += body[i];
      }
      ++i;
    }
    if (i >= body.size()) return false;
    ++i;  // closing quote
    out.push_back(std::move(value));
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i < body.size() && body[i] == ',') {
      ++i;
      afterComma = true;
      continue;
    }
    if (i < body.size() && body[i] == ']') return true;
    return false;
  }
}

}  // namespace form_fields
