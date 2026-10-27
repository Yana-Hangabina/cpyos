#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Single-byte text encodings. Used by the FB2 parser for the encoding named in
// its XML declaration, and by the plain-text reader, which has to guess because
// .txt carries no label.
namespace Fb2Encoding {

// Canonical names returned by detect() and accepted by every function here.
// Callers can persist the result and compare it later.
inline constexpr char UTF8[] = "utf-8";
inline constexpr char WINDOWS_1251[] = "windows-1251";
inline constexpr char KOI8_R[] = "koi8-r";
inline constexpr char WINDOWS_1252[] = "windows-1252";

// Returns a Unicode code point for a byte in a supported single-byte XML
// encoding, or -1 when either the encoding or byte is undefined.
int decodeByte(const char* encodingName, uint8_t value);

// True when this name resolves to a supported single-byte encoding. UTF-8 is
// multi-byte, so it is deliberately not "supported" here.
bool isSupported(const char* encodingName);

// Append one code point to `out` as UTF-8. Negative values become U+FFFD.
void appendUtf8(std::string& out, int codepoint);

// Transcode single-byte text to UTF-8. Unsupported encodings (including UTF-8
// itself) are copied through unchanged. One source byte always yields exactly one
// code point, which is what lets callers map a result offset back to a source
// offset by counting code points.
std::string toUtf8(const char* encodingName, const char* bytes, size_t length);

// Guess the encoding of a text sample, returning one of the canonical names
// above. `sample` should be a few KB from the start of the file: a UTF-8 BOM is
// recognised, otherwise the choice comes from the distribution of high bytes.
const char* detect(const char* sample, size_t length);

// Length of the byte-order mark at the start of this sample, or 0 when absent.
size_t bomLength(const char* sample, size_t length);

}  // namespace Fb2Encoding
