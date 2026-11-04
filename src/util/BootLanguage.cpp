#include "BootLanguage.h"

#if !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED)
#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "cpyosboot";
constexpr const char* kKey = "lang";
}  // namespace

void bootlang::remember(const uint8_t language) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return;
  // NVS wears out with writes; only touch it when the choice really changed.
  if (prefs.getUChar(kKey, 0xFF) != language) prefs.putUChar(kKey, language);
  prefs.end();
}

uint8_t bootlang::recall() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return 0xFF;
  const uint8_t language = prefs.getUChar(kKey, 0xFF);
  prefs.end();
  return language;
}

#else

void bootlang::remember(uint8_t) {}

uint8_t bootlang::recall() { return 0xFF; }

#endif
