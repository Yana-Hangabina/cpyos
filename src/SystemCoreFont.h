#pragma once

#include <cstdint>

// Tier-1 reading font provisioning.
//
// The core TrueType face lives compressed (CPFZ) in the data/spiffs partition
// at kPartitionOffset. On the first boot after a full install it is inflated to
// the SD card as a normal vector family, where SdCardFontSystem loads it with
// reader budgets. Keeping it out of app0 saves ~1.6 MB of OTA payload; keeping
// the UI subset in app0 means the system UI never waits for this step.
//
// If the carrier is absent (an app-only OTA that did not update the data
// partition) or the SD card cannot hold it, releaseToSd() returns false and the
// reader stays on the UI subset: the existing missing-glyph detection then
// offers the downloadable core pack.
namespace system_core_font {

// Partition-relative offset, read by scripts/generate_data_manifest.py.
inline constexpr uint32_t kPartitionOffset = 0x15C000;

inline constexpr const char* kFamilyName = "CPYOSCJKCore";
inline constexpr const char* kDir = "/fonts/CPYOSCJKCore";
inline constexpr const char* kTtfPath = "/fonts/CPYOSCJKCore/CPYOSCJKCore.ttf";
inline constexpr const char* kCrcPath = "/fonts/CPYOSCJKCore/.core-crc";
inline constexpr const char* kMarkerPath = "/fonts/CPYOSCJKCore/.cpyos-core";

// True when a valid core carrier is present in the data partition.
//
// Reads internal flash. On a PSRAM-stack task this returns false *without
// caching anything*, so a later internal-stack caller still gets the real
// answer -- see memory::guardInternalFlash() and Status::Problem::WrongTaskStack.
bool carrierPresent();

// Inflate the carrier and install it under /fonts/CPYOSCJKCore. Idempotent; a
// matching install is left untouched. Returns true when the SD file is usable.
bool releaseToSd();

// True when the installed SD file matches the carrier (size + CRC sidecar).
bool ready();

// No-card fallback: inflate the carrier and KEEP it in PSRAM as the reading face.
//
// Without this, "SD unavailable" means the core is entirely unavailable -- the reader
// drops to app0's 1,520-codepoint UI face and tofu for 拟/勾/综, even though the data
// partition carries those glyphs. The obvious alternative (register a stream source that
// reads the partition on each glyph fault) panics on this board: glyph rasterisation runs
// on the render task, whose stack is in PSRAM, and an internal-flash read disables the
// cache under it (AGENTS.md §43). So the bytes must be materialised once, from a task with
// an internal-RAM stack, and then served from RAM.
//
// Must be called from the main task, before any render task can ask for the face. The
// buffer is intentionally never freed: a font family descriptor outlives the caller that
// registered it (same reasoning as the hyphenation store), and this state only exists on a
// device that has no card to put the font on. Returns false -- with a loud log, never
// silently -- if the caller's stack is in PSRAM, PSRAM cannot fund it, or the carrier is
// absent or corrupt.
bool installResident();

// The resident buffer, when installResident() succeeded. `len` is the TTF size.
const uint8_t* residentData(uint32_t* len);

// True when /fonts/CPYOSCJKCore holds a face worth registering as a supplement,
// including one this build did not release (a wider downloaded or older core).
// ready() means "this build's carrier is in place"; it is the wrong gate for
// *using* a font, and using the wrong gate silently discarded a better face than
// the build could supply. See SystemCoreFontCheck.h facePresent().
bool faceOnSd();

// The tier this build's carrier declares ("3500", "8105"), or "unknown" for a carrier
// written before the field existed. Reporting only: nothing on this path branches on it.
// Meaningful after the evaluation has run (ready()/releaseToSd()/status()).
const char* tierLabel();

// Why the reading core is not in use, in one machine-readable place.
//
// Until now every failure on this path only produced an SD-sized absence of a
// font: the UI fell back to the embedded subset and drew tofu for characters the
// data partition demonstrably carried (or that a full install never wrote at all).
// Each stage below distinguishes "not flashed" from "flashed but not installed"
// from "installed and rejected", which are three different user answers.
struct Status {
  enum class Problem : uint8_t {
    None,            // ready, or nothing tried yet
    NoPartition,     // no data/spiffs partition at all
    BadCarrier,      // header absent/wrong magic/wrong version/implausible sizes
    PsramShort,      // the carrier could not be inflated
    SdUnavailable,   // no SD card, or the install write failed
    NotInstalled,    // carrier is fine, no matching SD file yet
    SidecarStale,    // size matches, marker missing or wrong (repaired when possible)
    ForeignCopy,     // /fonts/CPYOSCJKCore holds a different font; never overwritten, but used
    WrongTaskStack,  // never evaluated: the caller's stack is in PSRAM, so reading the
                     // carrier would panic. Distinct from BadCarrier on purpose -- the
                     // carrier may be perfectly fine, and "unknown" must not be reported
                     // or acted on as "missing" (see memory::guardInternalFlash).
  };

  bool carrierPresent = false;
  bool sdFilePresent = false;
  bool ready = false;
  // True when a core face is registered as a reading supplement, whether or not it is this
  // build's carrier. ready() alone would report "no core" on a device that is happily using
  // a wider downloaded or older face, which is exactly the confusion this struct exists to
  // remove (see facePresent() in SystemCoreFontCheck.h).
  bool faceInUse = false;
  uint32_t compressedSize = 0;
  uint32_t rawSize = 0;
  uint32_t rawCrc = 0;
  uint32_t sdSize = 0;
  // The carrier's own tier label, so a status page or log can say "this build ships the 3,500
  // core" without a size table. Static storage; safe to keep the pointer.
  const char* tier = "unknown";
  Problem problem = Problem::None;
};

// Cheap: one partition header read plus one file stat. It never inflates and never
// writes, but it does read internal flash -- on a PSRAM-stack task it reports
// WrongTaskStack and caches nothing, which is the honest "unknown", not "absent".
Status status();

// "carrier-present" / "foreign-copy" / ... for logs and the font status page.
const char* problemName(Status::Problem problem);

}  // namespace system_core_font
