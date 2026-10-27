#include "I18n.h"

#include <cstddef>
#include <cstring>

#include "I18nOffsetBuilder.h"
#include "I18nStrings.h"

// Same guard as I18nPack.cpp and WebAssets.cpp: the pack path allocates into
// PSRAM and reads a data partition, so a target without PSRAM must not select
// it. Without this the failure would be an unclear error inside heap_caps_*.
#if defined(CROSSPOINT_I18N_PACK) && !defined(BOARD_HAS_PSRAM)
#error "CROSSPOINT_I18N_PACK needs BOARD_HAS_PSRAM: the language pack is read into PSRAM"
#endif

#if defined(BOARD_HAS_PSRAM)
#if defined(__has_include)
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define I18N_HAS_HEAP_CAPS 1
#endif
#endif
#if !defined(I18N_HAS_HEAP_CAPS)
// A silent internal-RAM fallback would hide the exact bug this build flag is
// meant to prevent, so refuse to compile instead.
#error "BOARD_HAS_PSRAM is set but esp_heap_caps.h is unavailable"
#endif
#include "Logging.h"
#endif

#if defined(CROSSPOINT_I18N_PACK)
#include "I18nPack.h"
#endif

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

#if defined(CROSSPOINT_I18N_PACK)

namespace {

#if I18N_EMBEDDED_ZH
// Builds Chinese's offset table from the embedded bitmap. English keeps its
// flash table; Chinese is the only other language in the image on this target.
bool buildChineseOffsets(uint16_t* offsets) {
  const auto keyCount = static_cast<uint32_t>(StrId::_COUNT);
  const auto slot = static_cast<size_t>(Language::ZH_CN);
  return i18n_offsets::buildOffsets(SAME_AS_EN_ZH_CN, keyCount, OFFSETS_EN, STRINGS_ZH_CN_DATA,
                                    LANG_BLOB_SIZES[slot], offsets);
}
#endif

const char* failureReason() {
  // status() only covers the pack itself. A ready pack can still fail to
  // supply one language (absent entry, corrupt blob, or a transient PSRAM
  // allocation failure), and that reason was logged by loadLanguage().
  return i18n_pack::status() == i18n_pack::Validity::Ok ? "no usable table for this language"
                                                        : i18n_pack::statusText();
}

}  // namespace

bool I18n::adoptPackedLanguage(Language lang) {
  const auto* runtime = i18n_pack::runtime(lang);
  if (runtime == nullptr) return false;
  const auto slot = static_cast<size_t>(lang);
  _runtimeOffsets[slot] = runtime->offsets;
  _runtimeBlob[slot] = runtime->data;
  return true;
}

void I18n::init() {
  // Idempotent: main.cpp calls this once, but a second call must not leak a
  // second set of PSRAM tables.
  static bool built = false;
  if (built) {
    return;
  }
  built = true;

  const bool packReady = i18n_pack::init();

#if I18N_EMBEDDED_ZH
  {
    const auto slot = static_cast<size_t>(Language::ZH_CN);
    const auto keyCount = static_cast<uint32_t>(StrId::_COUNT);
    auto* offsets = static_cast<uint16_t*>(
        heap_caps_malloc(keyCount * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (offsets == nullptr) {
      LOG_ERR("I18N", "no PSRAM for the Chinese offset table; English fallback");
    } else if (!buildChineseOffsets(offsets)) {
      heap_caps_free(offsets);
      LOG_ERR("I18N", "Chinese offsets rejected (bitmap and blob disagree); English fallback");
    } else {
      _runtimeOffsets[slot] = offsets;
      _runtimeBlob[slot] = STRINGS_ZH_CN_DATA;
    }
  }
#endif

  if (i18n_pack::isPacked(_language) &&
      !(i18n_pack::loadLanguage(_language) && adoptPackedLanguage(_language))) {
    // The stored choice is deliberately left alone: a pack that is missing
    // because of a --skip-data flash must not rewrite the user's language. get()
    // falls back to English until a pack with this language is installed.
    LOG_ERR("I18N", "%s requested but %s; English fallback", getLanguageName(_language), failureReason());
  }

  LOG_INF("I18N", "language pack %s; %u language(s) in PSRAM; current=%s",
          packReady ? "ready" : i18n_pack::statusText(),
          static_cast<unsigned>(i18n_pack::loadedLanguageCount()), getLanguageName(_language));
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  if (_language != Language::EN) {
    const auto slot = static_cast<size_t>(_language);
    const uint16_t* offsets = _runtimeOffsets[slot];
    if (offsets == nullptr) {
      // Not loaded (missing pack, not initialised yet, or the language failed
      // verification): behave exactly like English rather than reading a
      // missing table.
      return i18n_offsets::resolve(STRINGS_EN_DATA, OFFSETS_EN, index, STRINGS_EN_DATA);
    }
    // For an all-English blob data is nullptr; every bit-15 entry resolves into
    // the English table, so the pointer is never dereferenced.
    return i18n_offsets::resolve(_runtimeBlob[slot], offsets, index, STRINGS_EN_DATA);
  }

  const LangStrings lang = getLanguageStrings(_language);
  return i18n_offsets::resolve(lang.data, lang.offsets, index, STRINGS_EN_DATA);
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;

  // Load eagerly so the first render after the picker closes already has the
  // new strings. On boot this runs before init(), which is fine: the pack
  // loader initialises itself and init() reuses that result.
  if (i18n_pack::isPacked(lang) && !(i18n_pack::loadLanguage(lang) && adoptPackedLanguage(lang))) {
    LOG_ERR("I18N", "cannot load %s: %s; English fallback", getLanguageName(lang), failureReason());
  }
}

bool I18n::isLanguagePackAvailable(Language lang) {
  if (static_cast<uint8_t>(lang) >= getLanguageCount()) return false;
  if (!i18n_pack::isPacked(lang)) return true;
  return i18n_pack::hasLanguage(lang);
}

const char* I18n::packStatusText() { return i18n_pack::statusText(); }

#elif defined(BOARD_HAS_PSRAM)

void I18n::init() {
  // Idempotent: main.cpp calls this once, but a second call must not leak a
  // second set of PSRAM tables.
  static bool built = false;
  if (built) {
    return;
  }
  built = true;

  const size_t keyCount = static_cast<size_t>(StrId::_COUNT);
  const size_t bytes = keyCount * sizeof(uint16_t);
  const size_t languageCount = static_cast<size_t>(getLanguageCount());
  size_t rebuilt = 0;
  size_t failed = 0;

  for (size_t i = 1; i < languageCount; ++i) {
    const LangStrings strings = getLanguageStrings(static_cast<Language>(i));
    auto* offsets =
        static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (offsets == nullptr) {
      ++failed;
      continue;
    }
    if (!i18n_offsets::buildOffsets(SAME_AS_EN_TABLES[i], static_cast<uint32_t>(keyCount),
                                    OFFSETS_EN, strings.data, LANG_BLOB_SIZES[i], offsets)) {
      heap_caps_free(offsets);
      ++failed;
      continue;
    }
    _runtimeOffsets[i] = offsets;
    ++rebuilt;
  }

  if (failed != 0) {
    LOG_ERR("I18N", "%u language offset table(s) failed; English fallback", static_cast<unsigned>(failed));
  }
  LOG_INF("I18N", "offset tables in PSRAM: %u/%u languages", static_cast<unsigned>(rebuilt),
          static_cast<unsigned>(languageCount - 1));
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  if (_language != Language::EN) {
    const uint16_t* offsets = _runtimeOffsets[static_cast<size_t>(_language)];
    if (offsets == nullptr) {
      // init() has not run yet, or this language failed verification: behave
      // exactly like English rather than reading a missing table.
      return i18n_offsets::resolve(STRINGS_EN_DATA, OFFSETS_EN, index, STRINGS_EN_DATA);
    }
    return i18n_offsets::resolve(getLanguageStrings(_language).data, offsets, index,
                                 STRINGS_EN_DATA);
  }

  // English, and every language on non-PSRAM targets, uses the generated table.
  const LangStrings lang = getLanguageStrings(_language);
  return i18n_offsets::resolve(lang.data, lang.offsets, index, STRINGS_EN_DATA);
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

bool I18n::isLanguagePackAvailable(Language lang) {
  (void)lang;
  return true;
}

const char* I18n::packStatusText() { return nullptr; }

#else  // no PSRAM

void I18n::init() {
  // Targets without PSRAM keep the generated tables in flash; nothing to do.
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // English, and every language on non-PSRAM targets, uses the generated table.
  const LangStrings lang = getLanguageStrings(_language);
  return i18n_offsets::resolve(lang.data, lang.offsets, index, STRINGS_EN_DATA);
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

bool I18n::isLanguagePackAvailable(Language lang) {
  (void)lang;
  return true;
}

const char* I18n::packStatusText() { return nullptr; }

#endif

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

bool I18n::isLanguageAvailable(Language lang) {
  if (static_cast<uint8_t>(lang) >= getLanguageCount()) return false;
#ifndef ENABLE_CHINESE_VERSION
  // CJK glyphs ship only with the Chinese SKU; the global build embeds Latin
  // fonts only, so Chinese strings render as garbled boxes even though ZH_CN's
  // strings are compiled in. Treat it as unavailable here.
  if (lang == Language::ZH_CN) return false;
#endif
  return true;
}
