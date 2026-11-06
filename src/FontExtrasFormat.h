#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace font_extras {

// Text manifest consumed by the on-device Font Extras screen. It is deliberately
// a small line-oriented format instead of JSON: it parses in a few KB of stack,
// has no allocator failure mode, and the host test can pin every rule without
// pulling ArduinoJson into the test build.
//
//   FEXT1
//   pack<TAB>id<TAB>kind<TAB>family<TAB>name<TAB>file<TAB>size<TAB>sha256<TAB>url
//
// kind      "fallback" (a vector font registered as a renderer fallback) or
//           "emoji" (a full emoji TTF used by EmojiFont instead of the pack).
// family    renderer family name for fallback packs; "-" for emoji packs.
// size      decimal bytes, 512 .. kMaxPackBytes.
// sha256    64 lowercase/uppercase hex characters.
// url       https:// only; GitHub release URLs are preferred so the OTA mirror
//           list can be reused.
constexpr size_t kMaxPacks = 12;
constexpr size_t kMaxManifestBytes = 16 * 1024;
constexpr size_t kMaxPackBytes = 32 * 1024 * 1024;
constexpr size_t kMaxIdBytes = 32;
constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxFamilyBytes = 32;
constexpr size_t kMaxFileBytes = 64;
constexpr size_t kMaxUrlBytes = 320;

enum class Kind : uint8_t { Fallback, Emoji };

struct Pack {
  char id[kMaxIdBytes + 1] = {};
  char name[kMaxNameBytes + 1] = {};
  char family[kMaxFamilyBytes + 1] = {};
  char file[kMaxFileBytes + 1] = {};
  char url[kMaxUrlBytes + 1] = {};
  uint64_t size = 0;
  uint8_t sha256[32] = {};
  Kind kind = Kind::Fallback;
};

// Parses one FEXT1 manifest. `count` is only written on success. On failure
// `error` receives a human-readable reason including the line number.
bool parseManifest(const char* data, size_t len, Pack* out, size_t capacity, size_t& count, char* error,
                   size_t errorCap);

bool parseSha256Hex(const char* text, uint8_t out[32]);

bool isValidId(const char* text);
bool isValidFamily(const char* text);
bool isValidFileName(const char* text);
bool isValidUrl(const char* text);

// Display helper shared by the activity and the serial diagnostics.
const char* kindName(Kind kind);
bool parseKind(const char* text, Kind& kind);

}  // namespace font_extras
