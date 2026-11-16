#include "base64.h"

#include <string>

namespace base64 {
namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

String encode(const uint8_t *data, size_t len) {
    String out;
    if (data == nullptr || len == 0) {
        return out;
    }
    out.reserve(((len + 2) / 3) * 4 + 1);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += kAlphabet[chunk & 0x3F];
    }
    if (i < len) {
        uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        const bool two = (i + 1) < len;
        if (two) {
            chunk |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += two ? kAlphabet[(chunk >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

String encode(const char *text) {
    if (text == nullptr) {
        return String();
    }
    return encode(reinterpret_cast<const uint8_t *>(text), std::char_traits<char>::length(text));
}

String encode(const String &text) { return encode(text.c_str()); }

}  // namespace base64

#include <cctype>

