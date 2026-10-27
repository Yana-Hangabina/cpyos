#pragma once

// Lazy, session-long loader for the Pinyin database.
//
// The table is about 349 KB — several times the flash the app image has left —
// so it never rides in the application. Two sources, in order:
//
//   1. `/pinyin.dat` on the SD card — the updatable copy the user can replace
//      without reflashing;
//   2. the bundled copy burnt into the data/spiffs partition by the two-image
//      flash command (see lib/Pinyin/README.md) — what makes a freshly flashed
//      device work with no card preparation at all.
//
// Either way the image is read into PSRAM on first use. Load happens on the
// main task and never falls back to internal RAM: without PSRAM there is no
// correct place to put it, so the feature is simply unavailable there (the C3
// builds), exactly like the JJWXC app's PSRAM gate.

#include <Memory.h>

#include "PinyinTables.h"

namespace pinyin {

class PinyinDb {
 public:
  static PinyinDb& shared();

  // Idempotent. Returns false when neither source has a database, PSRAM
  // cannot fund it, or validation (magic / structure / CRC) fails; callers
  // treat all three as "Pinyin features unavailable" and keep their old
  // behaviour. A card that exists but is corrupt falls through to the bundled
  // copy rather than failing outright.
  bool ensureLoaded();
  bool loaded() const { return tables_.valid(); }
  const PinyinTables& tables() const { return tables_; }
  void release();

 private:
  bool loadFromCard();
#if !defined(SIMULATOR)
  bool loadBundled();
#endif

  memory::ByteBuffer buffer_;
  PinyinTables tables_;
};

}  // namespace pinyin
