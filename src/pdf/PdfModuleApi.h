#pragma once

#include <cstdint>

#include "PdfModuleHost.h"

// The PDF engine lives in the dedicated `pdfmod` partition (or, on the Metalio
// Plus, on the SD card) and is executed in place.
//
// Compatibility model (v2).  The engine is *not* bound to one exact firmware
// build any more: a rebuild that does not change the contract below keeps the
// installed engine valid.  What both sides must agree on:
//
//   version    image/header layout (this file)
//   abi        PdfModuleOps function-table layout (append-only)
//   hostApi    PdfModuleHost service table (major exact, minor <=)
//   layoutKey  virtual-address / bss-gap constants compiled into both images
//
// `buildId` is the SHA-256 of the firmware ELF the engine was linked from.  It
// is recorded for provenance (logs, manifests, the publishing gate) and is
// deliberately NOT compared at load time: it changes on every rebuild, which is
// exactly what used to orphan a good engine.
//
// Layout of the partition:
//   0x00000  PdfModuleHeader
//   0x10000  code image (ELF .text, linked at kCodeVaddr)
//   ...      rodata image (ELF .rodata, linked at kRodataVaddr)
//   ...      data image (ELF .data, copied to kDataVaddr at load time)
// The module's .bss is zeroed by the loader at kDataVaddr + bssGap.

constexpr uint32_t kPdfModuleMagic = 0x31504d43u;  // "CPM1" little-endian
// v2 adds hostApi/layoutKey/engineRev/buildId.  Pre-v2 images are refused as
// "image format not supported"; there is no migration path by design.
constexpr uint32_t kPdfModuleVersion = 2;
// ABI 4 changes pdfmod_entry() to receive the host table.  Older engines cannot
// be run by this app and vice versa.
constexpr uint32_t kPdfModuleAbiVersion = 4;
constexpr uint32_t kPdfModuleHeaderBytes = 0x10000;
#if defined(CROSSPOINT_PDF_XIP_SD)
// S31 (Metalio E-Plus): the engine image lives on the SD card and is copied
// into PSRAM, so all three windows are arbitrary slots inside the
// 0x50000000..0x54000000 external-RAM window.  They sit above the app's static
// ext_ram and the PSRAM heap; the loader maps fresh physical pages there.
constexpr uint32_t kPdfModuleCodeVaddr = 0x53800000;
constexpr uint32_t kPdfModuleRodataVaddr = 0x53A00000;
constexpr uint32_t kPdfModuleDataVaddr = 0x53B00000;
#else
// The allocator's first free instruction slot for THIS app layout.  It moves
// when the app IROM grows (0x42D30000 -> 0x42D50000 on 2026-11-24); the loader
// reports the chosen address when it differs.
constexpr uint32_t kPdfModuleCodeVaddr = 0x42D50000;
constexpr uint32_t kPdfModuleRodataVaddr = 0x3DF00000;
// The mutable window lives high in the external-RAM VA space.  The allocator
// behind esp_mmu_map hands out the first free page range, and that range moved
// when the app image changed; a module linked at a fixed address cannot follow
// it.  ~14 MB in is well past the app's static ext_ram and the PSRAM heap, and
// is mapped raw by the loader.
constexpr uint32_t kPdfModuleDataVaddr = 0x3D800000;
#endif
constexpr uint32_t kPdfModuleDataBssGap = 0x20000;
constexpr const char* kPdfModulePartition = "pdfmod";

struct PdfModuleHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t abi;
  uint32_t hostApi;  // (major << 16) | minor
  uint32_t layoutKey;
  uint32_t headerBytes;
  uint32_t codeOffset;
  uint32_t entryOffset;
  uint32_t codeSize;
  uint32_t codeCrc32;
  uint32_t rodataOffset;
  uint32_t rodataSize;
  uint32_t rodataCrc32;
  uint32_t dataOffset;
  uint32_t dataSize;
  uint32_t dataCrc32;
  uint32_t bssSize;
  uint32_t engineRev;  // ordering key for "newest compatible engine"
  uint8_t buildId[32]; // sha256(firmware.elf); provenance, not compatibility
};

struct PdfModuleOps {
  uint32_t abi;
  uint32_t structSize;
  void* (*open)(const char* path, const char* password);
  void (*close)(void* handle);
  int (*needs_password)(void* handle);
  int (*wrong_password)(void* handle);
  int (*page_count)(void* handle);
  int (*page_has_text)(void* handle, int page);
  int (*extract_text)(void* handle, int page, char* out, int cap, int* hasImages);
  int (*outline_count)(void* handle);
  int (*outline_at)(void* handle, int index, char* title, int cap, int* page);
  int (*render_bmp)(void* handle, int page, const char* bmpPath, int width, int height, int cover);
  int (*write_blank)(void* handle, const char* bmpPath, int width, int height);
  void (*trim)(void* handle);
  int (*ensure_tile)(void* handle, int width);
  void (*set_display_list)(void* handle, int enabled);
  // ABI 3, last: lets the reader's typography option reach the extractor, which
  // is where a MuPDF block can be split back into the paragraphs it fused.
  void (*set_paragraph_split)(void* handle, int enabled);
};

namespace pdf_module_layout {
constexpr uint32_t mix(uint32_t hash, const uint32_t value) { return (hash ^ value) * 16777619u; }
constexpr uint32_t compute() {
  uint32_t h = 2166136261u;
  h = mix(h, kPdfModuleMagic);
  h = mix(h, kPdfModuleVersion);
  h = mix(h, kPdfModuleAbiVersion);
  h = mix(h, static_cast<uint32_t>(kPdfModuleHostApiMajor));
  h = mix(h, static_cast<uint32_t>(kPdfModuleHostApiMinor));
  h = mix(h, kPdfModuleHeaderBytes);
  h = mix(h, kPdfModuleCodeVaddr);
  h = mix(h, kPdfModuleRodataVaddr);
  h = mix(h, kPdfModuleDataVaddr);
  h = mix(h, kPdfModuleDataBssGap);
  // Struct sizes are deliberately NOT mixed in: sizeof depends on the host
  // compiler's pointer width, which would make the host-side tests disagree
  // with the device.  Table growth is already versioned by abi/hostApi.
  return h;
}
}  // namespace pdf_module_layout

// Both images compile this header, so the key changes only when a layout
// constant above changes -- never on an unrelated app rebuild.  The literal is
// the value the build scripts stamp into images; the static_assert keeps it in
// lockstep with the constants above so a hand edit cannot drift.
#if defined(CROSSPOINT_PDF_XIP_SD)
constexpr uint32_t kPdfModuleLayoutKey = 0xEE4B634Fu;
#else
constexpr uint32_t kPdfModuleLayoutKey = 0x7D84634Fu;
#endif
static_assert(pdf_module_layout::compute() == kPdfModuleLayoutKey,
              "update kPdfModuleLayoutKey when the module layout constants change");

// True when an image with this header can run on an app built from this tree.
constexpr bool pdfModuleCompatible(const PdfModuleHeader& h) {
  return h.version == kPdfModuleVersion && h.abi == kPdfModuleAbiVersion &&
         (h.hostApi >> 16) == static_cast<uint32_t>(kPdfModuleHostApiMajor) &&
         (h.hostApi & 0xFFFFu) <= static_cast<uint32_t>(kPdfModuleHostApiMinor) &&
         h.layoutKey == kPdfModuleLayoutKey;
}

// entry(host, ops): fills the app-side engine table; returns 0 on success.
extern "C" int pdfmod_entry(const PdfModuleHost* host, PdfModuleOps* ops);
