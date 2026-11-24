#pragma once

// Pre-erase and pre-load validation of a pdfmod image header.
//
// Two classes of failure, and they need different remedies:
//   * structural -- truncated/spliced/geometry broken: the file cannot run here
//   * compatibility -- intact image, but engineAbi/hostApi/layoutKey belong to a
//     different app contract: the file is fine and a matching engine must be
//     installed
// The loader classifies them so the reader can offer the update path instead of
// "SD 卡无 /pdfmod.bin"; the installer runs the same predicate before erasing.
//
// Pure: no ESP, no filesystem. Compiled by the app, the module loader and tests.
#include <cstddef>
#include <cstdint>

#include "PdfModuleApi.h"

namespace pdf_header {

enum class Status : uint8_t {
  Ok,
  BadMagic,
  BadVersion,
  AbiMismatch,
  HostApiMismatch,
  LayoutMismatch,
  CodeOffsetWrong,
  CodeEmpty,
  SectionsOverlap,   // rodata/data do not start after the section before them
  FileTooSmall,      // declared sections do not fit in the bytes we have
  PartitionTooSmall, // the image does not fit in the pdfmod partition
  EntryOutsideCode,
  BssTooLarge,
};

// True for "this is a valid engine, just for another app contract".  Those are
// fixed by downloading/installing the right image, not by power-cycling.
constexpr bool isCompatibilityStatus(const Status status) {
  return status == Status::AbiMismatch || status == Status::HostApiMismatch ||
         status == Status::LayoutMismatch;
}

// `fileBytes`      -- size of the image being inspected
// `partitionBytes` -- size of the partition, 0 when unknown (skip that check)
inline Status check(const PdfModuleHeader& h, const uint64_t fileBytes, const uint64_t partitionBytes) {
  if (h.magic != kPdfModuleMagic) return Status::BadMagic;
  if (h.version != kPdfModuleVersion) return Status::BadVersion;
  if (h.abi != kPdfModuleAbiVersion) return Status::AbiMismatch;
  if ((h.hostApi >> 16) != static_cast<uint32_t>(kPdfModuleHostApiMajor) ||
      (h.hostApi & 0xFFFFu) > static_cast<uint32_t>(kPdfModuleHostApiMinor)) {
    return Status::HostApiMismatch;
  }
  if (h.layoutKey != kPdfModuleLayoutKey) return Status::LayoutMismatch;
  // Code always follows the 64 KiB header sector, and .rodata/.data are aligned above it:
  // the loader maps these offsets directly, so an unexpected offset is a corrupt image.
  if (h.codeOffset != kPdfModuleHeaderBytes) return Status::CodeOffsetWrong;
  if (h.codeSize == 0) return Status::CodeEmpty;
  const uint64_t codeEnd = static_cast<uint64_t>(h.codeOffset) + h.codeSize;
  if (static_cast<uint64_t>(h.rodataOffset) < codeEnd) return Status::SectionsOverlap;
  const uint64_t rodataEnd = static_cast<uint64_t>(h.rodataOffset) + h.rodataSize;
  if (static_cast<uint64_t>(h.dataOffset) < rodataEnd) return Status::SectionsOverlap;
  if (h.entryOffset >= h.codeSize) return Status::EntryOutsideCode;
  // .bss lives in PSRAM, not in the file, but the loader memsets `bssSize` bytes at
  // kPdfModuleDataBssGap -- an absurd value is a corrupted header, and PSRAM is ~8 MB.
  if (h.bssSize > 4ull * 1024 * 1024) return Status::BssTooLarge;
  const uint64_t total = static_cast<uint64_t>(h.dataOffset) + h.dataSize;
  if (total > fileBytes) return Status::FileTooSmall;
  if (partitionBytes != 0 && total > partitionBytes) return Status::PartitionTooSmall;
  return Status::Ok;
}

// Human-readable reason, for the log line and the install dialog.
inline const char* reason(const Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::BadMagic: return "not a CPYOS PDF engine image";
    case Status::BadVersion: return "image format not supported";
    case Status::AbiMismatch: return "engine ABI does not match this firmware";
    case Status::HostApiMismatch: return "engine host API does not match this firmware";
    case Status::LayoutMismatch: return "engine memory layout does not match this firmware";
    case Status::CodeOffsetWrong: return "code section offset is wrong";
    case Status::CodeEmpty: return "code section is empty";
    case Status::SectionsOverlap: return "sections overlap";
    case Status::FileTooSmall: return "file is truncated";
    case Status::PartitionTooSmall: return "image is larger than the engine partition";
    case Status::EntryOutsideCode: return "entry point is outside the code section";
    case Status::BssTooLarge: return "zero-data size is absurd";
  }
  return "unknown";
}

}  // namespace pdf_header
