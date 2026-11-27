#pragma once

#include <cstddef>
#include <cstdint>

// What each feature of the firmware is allowed to hold, and who has to give it
// back. This is the "predictable" half of the memory question: the ledger says
// who holds what right now, this table says whether that was the deal.
//
// Two rules are stated here as pure functions because both were broken in the
// field, once each, and neither was visible until someone read a log line by
// hand:
//
//   1. A feature that is released to make room for the radio must have an owner
//      that puts it back. The 0.8.2 tofu report was this rule broken for the font
//      supplements (docs/plans/... §20): NetworkStartup::prepare() took the memory
//      and nothing gave it back, so every screen after a network entry drew from
//      app0's ~1,500-codepoint UI subset.
//   2. A declared limit that nobody measures is decoration. Entries therefore
//      carry whether the limit is measured or asserted, and the host suite prints
//      the unmeasured ones instead of letting them read as verified.
//
// Kept free of HAL/ESP includes so test/memory_contracts/ can enumerate every
// entry without a device.
namespace memory::contract {

// Who restores a released feature. One owner per mechanism, not one per feature:
// the point of the pairing is that the releaser cannot forget, and a per-feature
// callback list is a place to forget.
enum class Restorer : uint8_t {
  None = 0,        // never released for the radio, so nothing to pair
  RenderMemory = 1,  // memory::yield / font_restore pairing, run by the main loop
  SelfLimited = 2,   // the feature re-acquires on its own next use (lazy loaders)
};

enum class Release : uint8_t {
  Never = 0,
  ForNetwork = 1,
};

struct Entry {
  const char* feature;
  // The cap the code enforces today, in bytes. 0 means "no cap exists", which is
  // itself the finding, not a missing value.
  uint32_t declaredLimitBytes;
  // The largest single contiguous block this feature can ask for. Internal-RAM
  // requests are the ones that fail silently, so this column is the one a network
  // incident is read against.
  uint32_t largestAskBytes;
  Release releases;
  Restorer restorer;
  bool measured;  // true once a device trace backs the numbers above
};

// The table. Limits are quoted from the code that enforces them, so a change to a
// budget without a change here is a gate failure (test/memory_contracts).
inline constexpr Entry kEntries[] = {
    // lib/hal + display: the framebuffer planes, allocated once at boot and held.
    // The device dump found four separate 103,968 B blocks -- two from
    // EpdiyLcdDriver::begin, one from epdiyLcdBegin and one from
    // FreeInkDisplay::allocFrameBufferStorage -- not the one block the first
    // version of this table assumed. The limit is the sum, the ask one plane.
    // This is the reason "8 MB PSRAM" is really ~5 MB.
    {"display.framebuffer", 4u * 103968u, 103968u, Release::Never, Restorer::None, true},
    // Home's decoded-cover block and the 16-level bitmap scratch: both small
    // enough to be forgotten, both observed holding real PSRAM.
    {"ui.coverCache", 33302u, 33302u, Release::Never, Restorer::None, true},
    {"render.grayScratch", 14592u, 14592u, Release::Never, Restorer::None, true},
    // The band-diff snapshot: a whole extra framebuffer that exists only while an
    // interface uses partial refresh. It showed up in the dump because the text
    // settings page turns it on and off, which is exactly the kind of conditional
    // holder a static reading of the code misses.
    {"render.bandSnapshot", 103968u, 103968u, Release::Never, Restorer::None, true},
    // epdiy's high-level buffers: the same dump attributed 1,663,488 B of PSRAM to
    // epd_hl_init. That is the number a "why is half of PSRAM gone at boot"
    // question actually needs, and it is the first thing the ledger answered that
    // no log line had ever said.
    {"display.hlBuffers", 1663488u, 831744u, Release::Never, Restorer::None, true},
    // epdiy's LCD path needs ~45 KB of *contiguous* internal RAM per frame push;
    // this is the ask that fails as a black screen with no message (AGENTS §3).
    // Declared from the documented requirement, not from the ledger: those
    // allocations never reach the wrapped entry points, and the internal gap in a
    // dump is where they live. A limit the tool cannot see must not be marked
    // measured, which is why this row says false while the two above say true.
    {"display.scanInternal", 45056u, 45056u, Release::Never, Restorer::None, false},
    // Non-CJK UI scripts inflated from the app0 carrier. Not released for the
    // radio (ui_script_font::release() has no caller), so there is nothing to
    // pair; it looked like it vanished in the first dump only because the table
    // evicted its row, which is what the steal counter is for.
    {"ui.scriptSupplement", 49332u, 49332u, Release::Never, Restorer::None, true},
    // The ledger's own table: static internal RAM, the region it exists to audit.
    // Behind the same macro as the wrappers, so an unwired build (every release
    // image, C3, the simulator) pays nothing for it.
    {"memory.ledgerTable", 4152u, 4096u, Release::Never, Restorer::None, true},
    // SD/vector reading face: glyph cache at the reading budget.
    {"font.readerFace", 1024u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},
    // The same face while the text-settings preview is up (128 KB / 512 glyphs).
    {"font.previewFace", 128u * 1024u, 128u * 1024u, Release::ForNetwork, Restorer::SelfLimited, true},
    // A family's resident TTF copies, capped per family rather than per file.
    {"font.familyResident", 4u * 1024u * 1024u, 4u * 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},
    // Streaming prefix kept for a font that does not fit resident.
    {"font.streamPrefix", 1024u * 1024u, 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, true},
    // The released reading core on the card, loaded once into PSRAM.
    {"font.systemPack", 3u * 1024u * 1024u, 3u * 1024u * 1024u, Release::ForNetwork, Restorer::RenderMemory, false},
    // Emoji face plus its glyph cache; the SD full pack is the large case.
    {"font.emoji", 844596u + 96u * 1024u, 844596u, Release::Never, Restorer::None, true},
    // i18n blob for the active language, plus the offset tables that are never
    // freed because tr() hands out const char*.
    {"i18n.activeLanguage", 19089u, 19089u, Release::Never, Restorer::None, true},
    {"i18n.packedTables", 439u * 1024u, 439u * 1024u, Release::Never, Restorer::None, false},
    // Web assets are read into PSRAM for the session and released on stop.
    {"web.pack", 157876u, 157876u, Release::Never, Restorer::None, true},
    // Hyphenation automaton, one language at a time, never evicted.
    {"hyphenation.language", 206259u, 206259u, Release::Never, Restorer::None, true},
    // Task stacks that live in PSRAM. Listed because a stack is the one
    // allocation whose failure is an abort rather than a nullptr.
    {"task.render", 32u * 1024u, 32u * 1024u, Release::Never, Restorer::None, true},
    {"task.jjwxcJob", 128u * 1024u, 128u * 1024u, Release::Never, Restorer::None, true},
    {"task.jjwxcComment", 32u * 1024u, 32u * 1024u, Release::Never, Restorer::None, true},
    {"task.browserFetch", 96u * 1024u, 96u * 1024u, Release::Never, Restorer::None, true},
    {"task.cacheService", 128u * 1024u, 128u * 1024u, Release::Never, Restorer::None, true},
    // The browser's page buffer: 2 MB of contiguous PSRAM, and the reason the
    // reader face is dropped before a fetch (AGENTS §59.10).
    {"browser.pageBuffer", 2u * 1024u * 1024u, 2u * 1024u * 1024u, Release::Never, Restorer::None, true},
    // JJWXC container caps. These are the "must not be relaxed" numbers from
    // AGENTS §7: 1500 chapters x 144 B and 300 shelf rows x 432 B.
    {"jjwxc.chapters", 1500u * 144u, 1500u * 144u, Release::Never, Restorer::None, true},
    {"jjwxc.shelfRows", 300u * 432u, 300u * 432u, Release::Never, Restorer::None, true},
    // TLS: two session slots, contexts in PSRAM since 0.8.2. The largest single
    // ask is not yet measured on device, so it is declared unmeasured rather than
    // guessed: the number that would have to be wrong is the one that decides
    // whether a handshake succeeds.
    {"network.tlsSessions", 0u, 0u, Release::Never, Restorer::None, false},
    // Reader page cache (the three AA planes plus section scratch): released for
    // the radio by prepare(), and re-built lazily by the reader on next use.
    {"reader.pageCache", 0u, 0u, Release::ForNetwork, Restorer::SelfLimited, false},
};

inline constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// A feature released for the radio with no restorer is the 0.8.2 bug in table
// form: the memory goes, and the screen changes under the user with no path back.
constexpr bool paired(const Entry& entry) {
  return entry.releases != Release::ForNetwork || entry.restorer != Restorer::None;
}

// The known, deliberate debt. Adding a releasable feature here without a restorer
// turns the suite red; removing debt means deleting its name from this list, which
// is the only way this file stays honest about what is still unpaired.
inline constexpr const char* kKnownUnpaired[] = {};
inline constexpr size_t kKnownUnpairedCount = 0;

// Order in which features should yield, largest declared limit first. The radio
// needs a contiguous block, so the biggest releasable holder is the first
// candidate; releasing three small ones to save one large ask is how the reclaim
// ends up costing more than it buys.
constexpr size_t yieldOrder(const Entry* entries, size_t count, size_t* out, size_t cap) {
  size_t written = 0;
  for (size_t pass = 0; pass < count && written < cap; ++pass) {
    size_t best = count;
    for (size_t i = 0; i < count; ++i) {
      bool taken = false;
      for (size_t j = 0; j < written; ++j) {
        if (out[j] == i) {
          taken = true;
          break;
        }
      }
      if (taken || entries[i].releases != Release::ForNetwork) continue;
      if (best == count || entries[i].declaredLimitBytes > entries[best].declaredLimitBytes) best = i;
    }
    if (best == count) break;
    out[written++] = best;
  }
  return written;
}

// Does an observation fit the declaration? An entry with no declared limit can
// only ever be reported, never confirmed, which is why `measured` is part of the
// answer rather than a comment.
constexpr bool withinDeclared(const Entry& entry, const uint32_t observedBytes) {
  if (entry.declaredLimitBytes == 0) return false;
  return observedBytes <= entry.declaredLimitBytes;
}

constexpr const char* name(const Restorer restorer) {
  switch (restorer) {
    case Restorer::None:
      return "none";
    case Restorer::RenderMemory:
      return "render-memory";
    case Restorer::SelfLimited:
      return "self-limited";
  }
  return "unknown";
}

constexpr const char* name(const Release release) {
  return release == Release::ForNetwork ? "for-network" : "never";
}

}  // namespace memory::contract
