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
bool carrierPresent();

// Inflate the carrier and install it under /fonts/CPYOSCJKCore. Idempotent; a
// matching install is left untouched. Returns true when the SD file is usable.
bool releaseToSd();

// True when the installed SD file matches the carrier (size + CRC sidecar).
bool ready();

}  // namespace system_core_font
