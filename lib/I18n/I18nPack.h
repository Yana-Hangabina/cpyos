#pragma once

// Loader for the data-partition language pack.
//
// CROSSPOINT_I18N_PACK builds (Read Pico) keep English -- and Chinese when
// I18N_EMBEDDED_ZH is 1 -- in the firmware image and read every other
// language's blob + same-as-English bitmap from build/i18n_pack.dat. This is
// what keeps ~330 KB of translations out of a 6.4 MB app0 slot.
//
// Lifetime rules, because tr() returns `const char*` that callers cache for the
// rest of the session (settings rows, the language picker, web-server workers):
//   * a successfully loaded language is NEVER released -- not on a switch, not
//     on deep sleep (the device reboots, which is the natural end of a session);
//   * a permanent failure (CRC, missing language, offsets rejected) is latched
//     per language so a broken pack does not re-read the partition on every
//     switch;
//   * an allocation failure is NOT latched: PSRAM pressure is transient, and
//     turning one bad moment into a session-long refusal would be a decision
//     made from a moment that has already passed.
//
// Threading: init() and loadLanguage() read internal flash, which disables the
// caches. They must run on the main task or the UI task (internal stack), never
// on the render task whose stack is in PSRAM. The loader refuses with a loud
// error instead of letting ESP-IDF assert. The tr() hot path never touches the
// partition: it reads only the cached pointers below.

#include <cstdint>

#include "I18nKeys.h"

// Partition-relative offset of the pack inside data/spiffs. Single source of
// truth: scripts/generate_data_manifest.py reads this constant (as text) to
// build data_manifest.txt, so the manifest and the loader cannot drift.
inline constexpr uint32_t I18N_PACK_PARTITION_OFFSET = 0x100000;

#if defined(CROSSPOINT_I18N_PACK)

#include "I18nPackFormat.h"

namespace i18n_pack {

// One language's runtime tables. `data` is the blob (PSRAM, or nullptr for an
// all-English blob), `offsets` is the rebuilt uint16 table (PSRAM).
struct RuntimeLanguage {
  const char* data = nullptr;
  const uint16_t* offsets = nullptr;
};

// Validates the pack header and index once. Idempotent. Safe to call from any
// non-PSRAM-stack task. Returns false and leaves a reason in status() on
// failure; the device keeps running with English.
bool init();

// True once init() has validated the pack.
bool ready();

// Reason the pack is unusable, or Ok.
Validity status();

// Human-readable status for logs; nullptr when the pack is ready.
const char* statusText();

// True when this language's strings live in the pack rather than the image.
// English is never packed; Chinese is not packed while this build embeds it
// (I18N_EMBEDDED_ZH), and is treated as packed when it does not, so a build
// without Chinese reports it unavailable instead of serving English.
bool isPacked(Language lang);

// True when the pack is ready and contains this language. Once init() has
// validated the pack on an internal-stack task this reads only the cached
// index; before that it calls init(), which reads internal flash, so the first
// call must not come from the render task.
bool hasLanguage(Language lang);

// Loads and caches a packed language. Returns false when the caller must fall
// back to English (pack missing/corrupt, language absent, allocation failed, or
// called from a PSRAM-stack task).
bool loadLanguage(Language lang);

// Cached tables, or nullptr when the language has not been loaded.
const RuntimeLanguage* runtime(Language lang);

// Number of languages loaded this session (for the boot log and tests).
uint32_t loadedLanguageCount();

}  // namespace i18n_pack

#endif  // CROSSPOINT_I18N_PACK
