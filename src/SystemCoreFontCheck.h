#pragma once

#include <cstdint>

// What an installed reading-core file means, decided apart from any storage.
//
// The shipped behaviour was: compare the SD file's size to the carrier, then trust a
// sidecar CRC, and otherwise conclude "no core". Two real failures came from that.
// (1) The sidecar write was never checked, so a good face with a lost marker read as
// absent -- the reader fell back to the embedded UI subset and drew tofu for
// characters the partition actually carried. (2) The downloadable 8,105 pack and this
// build's smaller carrier share one family name and one path, so "size differs" meant
// "re-inflate over the user's download". Deciding the two cases apart, and refusing to
// write on the second one, is the whole point of this header.
//
// Kept free of HAL/ESP includes so a host test can enumerate every combination;
// src/SystemCoreFont.cpp maps storage results onto Facts and acts on the Verdict.
namespace system_core_font {

enum class Verdict : uint8_t {
  Ready,           // marker agrees with the carrier: use it, write nothing
  RepairMarker,    // the bytes are the carrier's, the marker was lost: rewrite it, use it
  NotInstalled,    // no file, or no card: inflate and install the carrier
  ForeignSize,     // a different-length face: the user's wider download -- never overwrite
  ForeignContent,  // same length, different CRC: not ours -- never overwrite
  Unreadable,      // the content check could not run: refuse to certify either way
};

struct Facts {
  bool storageReady = false;
  bool fileExists = false;
  bool sizeMatches = false;    // SD file length == carrier rawSize
  bool markerPresent = false;  // the .core-crc sidecar parsed
  bool markerMatches = false;  // ... and equals the carrier rawCrc
  bool contentReadable = false;
  bool contentMatches = false;  // streamed CRC-32 of the file == carrier rawCrc
};

constexpr Verdict classify(const Facts& facts) {
  if (!facts.storageReady || !facts.fileExists) return Verdict::NotInstalled;
  if (!facts.sizeMatches) return Verdict::ForeignSize;
  if (facts.markerPresent && facts.markerMatches) return Verdict::Ready;
  // A missing or stale marker proves nothing about the bytes, and proving them costs
  // one streamed CRC. That is the cheap direction to be wrong in: repairing a marker
  // is free, discarding a working font is what users report as "still missing".
  if (!facts.contentReadable) return Verdict::Unreadable;
  if (!facts.contentMatches) return Verdict::ForeignContent;
  return Verdict::RepairMarker;
}

// True when the carrier may be written over what is on the card. Foreign faces are
// left alone: keeping a wider download working beats restoring a narrower one.
//
// The earlier comment here claimed the font-extra path "already registers that family on
// its own", which is only true for a face installed *through* FontExtras. A face that got
// onto the card another way -- released by a firmware build with a wider tier, restored from
// a backup, copied by hand -- is in neither the installed list nor the carrier verdict, so
// refusing to overwrite it used to also mean refusing to read it, and the device drew tofu
// while holding a better font than the one it wanted. facePresent() below is what closes
// that hole: never overwrite, but always use.
constexpr bool mayInstall(Verdict verdict) {
  return verdict == Verdict::NotInstalled || verdict == Verdict::Unreadable;
}

constexpr bool usable(Verdict verdict) {
  return verdict == Verdict::Ready || verdict == Verdict::RepairMarker;
}

// True when the card holds a readable core face under /fonts/CPYOSCJKCore, whether or not
// it is this build's carrier. This is the gate for *registering* it as a supplement; the
// gate for writing it is mayInstall(). The two sets are disjoint by construction: a verdict
// never both installs and reports a face it refuses to overwrite.
constexpr bool facePresent(Verdict verdict) {
  return verdict == Verdict::Ready || verdict == Verdict::RepairMarker ||
         verdict == Verdict::ForeignSize || verdict == Verdict::ForeignContent;
}

// --- carrier self-description ---------------------------------------------------------
//
// The published blob is named cjk_core.dat for every tier, because devices on 0.8.0 ask
// for that name and the tier that fits depends on the partition table (§15.13, §15.9). A
// name shared by two contents is exactly the shape that produced the earlier incident, so
// the generator also writes the tier into the header's reserved field: the blob then says
// what it is, and any reader -- the device, exp/verify_device_packs.py, a person with a
// hexdump -- can check the content against the manifest instead of trusting which script
// ran last.
//
// Reporting only. A carrier with a missing or absurd label is still the same bytes, and the
// size/CRC/version checks already decide whether to use it; making a label load-bearing
// would let a bad tag turn a working font into "no core", which is the bug this whole file
// exists to fix.

inline constexpr uint32_t kTierFieldSize = 8;

// Where the label sits in a CPFZ carrier. The generator writes it, docs/file-formats.md
// documents it, and exp/verify_device_packs.py reads it; a test compares all three against
// this value, because a device reading the wrong four bytes would report "untagged" for a
// perfectly labelled font -- a quiet, harmless-looking failure of exactly the kind this
// section of the plan exists to remove.
inline constexpr uint32_t kTierFieldOffset = 0x14;

struct TierLabel {
  char text[kTierFieldSize + 1] = {};
  bool known = false;
};

inline constexpr bool isTierChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         c == '.' || c == '_' || c == '-';
}

// Accepts an ASCII label NUL-padded to kTierFieldSize. Empty, unpadded, or non-printable
// content yields known=false: carriers written before the tag existed are all zero, and
// those must read as "unknown", never as a tier and never as a fault.
//
// uint32_t rather than size_t on purpose: this header is included by a host test and by
// firmware alike and carries no <cstddef>.
constexpr TierLabel readTierLabel(const uint8_t* field) {
  TierLabel out;
  if (!field) return out;
  uint32_t length = 0;
  while (length < kTierFieldSize && field[length] != 0) ++length;
  if (length == 0) return out;
  for (uint32_t i = 0; i < kTierFieldSize; ++i) {
    // The label bytes must be label characters; the padding must be zero. Checking the
    // padding for printability instead would reject every real carrier.
    if (i >= length) {
      if (field[i] != 0) return out;
      continue;
    }
    if (!isTierChar(static_cast<char>(field[i]))) return out;
  }
  for (uint32_t i = 0; i < length; ++i) out.text[i] = static_cast<char>(field[i]);
  out.known = true;
  return out;
}

}  // namespace system_core_font
