#pragma once

#include <cstddef>
#include <cstdint>

// Host services handed to the PDF engine at pdfmod_entry().
//
// Why a table instead of absolute symbol addresses: the engine used to be
// linked with every app global symbol resolved to a fixed address, so any
// rebuild that moved a function invalidated a perfectly good engine.  The
// table makes the contract explicit and append-only: a module only ever calls
// the prefixes it knows (structSize + minor version), so an app rebuild that
// does not touch this struct keeps the engine valid.
//
// Rules for changing this file:
//   1. New services go at the END and bump kPdfModuleHostApiMinor.
//   2. Changing the signature/semantics of an existing entry bumps the major
//      and every engine must be rebuilt; the loader compares major exactly.
//   3. The struct is compiled by both the app and the module; keep it free of
//      Arduino/ESP headers.

constexpr uint16_t kPdfModuleHostApiMajor = 1;
constexpr uint16_t kPdfModuleHostApiMinor = 0;
constexpr uint32_t kPdfModuleHostApi = (static_cast<uint32_t>(kPdfModuleHostApiMajor) << 16) |
                                       static_cast<uint32_t>(kPdfModuleHostApiMinor);

// Subset of MALLOC_CAP_* the engine uses.  Values match ESP-IDF so the app can
// forward them to heap_caps_* unchanged.
enum : uint32_t {
  kPdfHostCap8Bit = 1u << 2,
  kPdfHostCapSpiram = 1u << 10,
};

struct PdfModuleHost {
  uint32_t abi;        // kPdfModuleHostApi the app was built for
  uint32_t structSize; // sizeof(PdfModuleHost); the module reads only what fits

  // Heap: the engine must live in PSRAM, and failure is reported as nullptr.
  void* (*heap_alloc)(size_t bytes, uint32_t caps);
  void* (*heap_aligned_alloc)(size_t alignment, size_t bytes, uint32_t caps);
  void* (*heap_realloc)(void* ptr, size_t bytes, uint32_t caps);
  void (*heap_free)(void* ptr);
  size_t (*heap_allocated_size)(const void* ptr);
  size_t (*heap_free_size)(uint32_t caps);
  size_t (*heap_largest_free_block)(uint32_t caps);
  size_t (*heap_total_size)(uint32_t caps);

  // Logging and scheduling.  `log` appends its own newline (same contract as
  // the app's logPrintf).
  void (*log)(const char* level, const char* origin, const char* format, ...);
  uint32_t (*millis)();
  void (*delay_ms)(uint32_t ms);
  void (*backtrace)(int depth);

  // Storage.  Opaque handles, byte-level only: the engine never sees HalFile or
  // SdFat.  `file_read` returns bytes read, or -1 on error.
  void* (*file_open_read)(const char* path);
  void* (*file_open_write)(const char* path);
  int (*file_read)(void* handle, void* dst, size_t bytes);
  int (*file_write)(void* handle, const void* src, size_t bytes);
  int (*file_seek)(void* handle, uint64_t offset);
  uint64_t (*file_size)(void* handle);
  void (*file_close)(void* handle);
  bool (*file_remove)(const char* path);

  // Asset hooks used by the patched MuPDF sources.
  const unsigned char* (*cmap_load)(const char* name, size_t* size);
  int (*font_cache_begin)();
  int (*font_cache_write)(const void* data, unsigned len);
  int (*font_cache_commit)(const char* name, char* out, unsigned cap);
  void (*font_cache_abort)();
  int (*font_cache_open)(const char* path);
  long (*font_cache_size)(int handle);
  long (*font_cache_read)(int handle, unsigned long offset, void* buffer, unsigned long len);
  void (*font_cache_close)(int handle);
};
