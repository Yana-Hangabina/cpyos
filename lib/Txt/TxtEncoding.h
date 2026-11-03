#pragma once

#include <cstddef>
#include <cstdint>

namespace txt_encoding {

enum class Encoding : uint8_t {
  Unknown = 0,
  Utf8 = 1,
  Gbk = 2,
  Utf16Le = 3,
  Utf16Be = 4,
};

struct TranscodeResult {
  size_t rawLength;
  size_t utf8Length;
};

bool isSerializedValueValid(uint8_t value);
bool isSupported(Encoding encoding);
Encoding detect(const uint8_t* data, size_t length, bool finalChunk);

// Uses one buffer for both source and destination. The caller must provide
// enough capacity for rawLength + the UTF-8 output.
TranscodeResult transcodeGbkInPlace(uint8_t* buffer, size_t rawLength, size_t capacity, bool finalChunk);

// Encoding-agnostic variant. UTF-16LE/BE (with a BOM, the only form detect()
// accepts) and GBK are converted to UTF-8; UTF-8/Unknown return {0, 0}.
TranscodeResult transcodeInPlace(uint8_t* buffer, size_t rawLength, size_t capacity, bool finalChunk,
                                 Encoding encoding);

// Encodes one code point as UTF-8 into `output` (at least 4 bytes) and returns
// the byte count. Exposed so streaming readers (TxtToHtml) can decode UTF-16
// unit by unit without a second table.
size_t encodeUtf8(uint32_t codepoint, uint8_t* output);

// Maps a consumed UTF-8 prefix back to bytes in the original GBK stream.
size_t gbkSourceLength(const uint8_t* utf8, size_t utf8Length);

// Encodes UTF-8 text into the two-byte GBK subset. Intended for query strings the
// server echoes back into a GB18030 page: sending UTF-8 bytes there produces a page
// that is neither valid UTF-8 nor valid GBK. Returns false (writing nothing) when a
// code point has no GBK form or the output does not fit, so the caller can fall
// back to UTF-8 rather than truncating or substituting silently.
bool encodeGbkFromUtf8(const char* utf8, size_t utf8Length, uint8_t* output, size_t outputCapacity,
                       size_t& outputLength);

}  // namespace txt_encoding
