#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Prepare the runtime tables (PSRAM targets only). On CROSSPOINT_I18N_PACK
  // builds this validates the data-partition pack and loads the current
  // language; other PSRAM targets rebuild every offset table from the
  // same-as-English bitmaps. Idempotent; a language that fails verification
  // falls back to English. Call once after the settings (and therefore the
  // language) are loaded and before any UI is rendered.
  void init();

  // Get localized string by ID.
  //
  // The returned pointer is valid for the rest of the session: packed language
  // blobs and their offset tables are never released, because callers (settings
  // rows, the language picker, web-server workers) cache the pointer. When a
  // packed language is missing or unusable this returns the English string.
  // O(1), no allocation, no lock.
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }

  // Selects a language and, on a pack build, loads its strings into PSRAM.
  // Must run on the main or UI task (internal stack), never on the render task.
  // The selection is kept even when the pack cannot provide the strings; get()
  // then falls back to English and SETTINGS.language is left untouched.
  void setLanguage(Language lang);
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // True if this build can actually render `lang`. A language whose strings are
  // compiled in but whose glyphs are not (e.g. ZH_CN in the non-CJK global
  // build) would render as garbled boxes, so it is reported as unavailable.
  static bool isLanguageAvailable(Language lang);

  // True if selecting `lang` will produce its strings. Always true on builds
  // without the data-partition pack; on a pack build, false when the pack is
  // missing/corrupt or does not carry this language. Deliberately separate from
  // isLanguageAvailable() so the glyph rule and the storage rule never get
  // conflated, and never rewrites the stored language choice.
  static bool isLanguagePackAvailable(Language lang);

  // Why the pack is unusable (nullptr when it is ready or this build embeds
  // every language). Used by logs and the language picker.
  static const char* packStatusText();

 private:
  I18n() : _language(Language::EN) {}

  Language _language;
#if defined(BOARD_HAS_PSRAM)
  // Offset tables rebuilt by init(); nullptr means "not built, use English".
  const uint16_t* _runtimeOffsets[static_cast<size_t>(Language::_COUNT)] = {};
#endif
#if defined(CROSSPOINT_I18N_PACK)
  // Blob a loaded language resolves into. English and Chinese point at their
  // image arrays; packed languages point at PSRAM that is never freed.
  const char* _runtimeBlob[static_cast<size_t>(Language::_COUNT)] = {};

  // Copies a successfully loaded packed language's pointers into the members
  // get() reads. Returns false when the loader has no cached table.
  bool adoptPackedLanguage(Language lang);
#endif
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
