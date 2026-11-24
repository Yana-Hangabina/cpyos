#pragma once

// Module-side shim: the engine source (PdfDocument.cpp) is written against the
// firmware's HalFile/heap/logging names.  Inside the module those names are
// redirected to the host table, so the engine never links against app symbols
// by absolute address.  Only compiled into build/pdfmod.bin.

#if defined(CROSSPOINT_PDF_MODULE)

#include "PdfModuleHost.h"

#include <cstddef>
#include <cstdint>

extern const PdfModuleHost* g_pdf_host;

#if !defined(MALLOC_CAP_8BIT)
#define MALLOC_CAP_8BIT kPdfHostCap8Bit
#endif
#if !defined(MALLOC_CAP_SPIRAM)
#define MALLOC_CAP_SPIRAM kPdfHostCapSpiram
#endif

// Distinct names + macros: Arduino.h (force-included) may declare the real
// heap_caps_* with C linkage, and defining a C++ inline with the same name
// afterwards would either conflict or silently leave the library call in place.
inline void* pdfhost_heap_malloc(const size_t bytes, const uint32_t caps) {
  return g_pdf_host->heap_alloc(bytes, caps);
}
inline void* pdfhost_heap_realloc(void* ptr, const size_t bytes, const uint32_t caps) {
  return g_pdf_host->heap_realloc(ptr, bytes, caps);
}
inline void pdfhost_heap_free(void* ptr) { g_pdf_host->heap_free(ptr); }
inline size_t pdfhost_heap_allocated_size(const void* ptr) { return g_pdf_host->heap_allocated_size(ptr); }
inline size_t pdfhost_heap_free_size(const uint32_t caps) { return g_pdf_host->heap_free_size(caps); }
inline size_t pdfhost_heap_largest(const uint32_t caps) { return g_pdf_host->heap_largest_free_block(caps); }
inline size_t pdfhost_heap_total(const uint32_t caps) { return g_pdf_host->heap_total_size(caps); }

#define heap_caps_malloc pdfhost_heap_malloc
#define heap_caps_realloc pdfhost_heap_realloc
#define heap_caps_free pdfhost_heap_free
#define heap_caps_get_allocated_size pdfhost_heap_allocated_size
#define heap_caps_get_free_size pdfhost_heap_free_size
#define heap_caps_get_largest_free_block pdfhost_heap_largest
#define heap_caps_get_total_size pdfhost_heap_total

// Arduino.h is force-included by the app compile command, so these have to be
// macros: they replace the inline millis()/delay() definitions picked up from
// that header.
#undef millis
#define millis() g_pdf_host->millis()
#undef delay
#define delay(ms) g_pdf_host->delay_ms(static_cast<uint32_t>(ms))

#define esp_backtrace_print(depth) g_pdf_host->backtrace(depth)

#define LOG_ERR(origin, format, ...) g_pdf_host->log("ERR", origin, format "\n", ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) g_pdf_host->log("INF", origin, format "\n", ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) \
  do {                               \
  } while (0)

// Minimal HalFile/HalStorage surface used by the engine.  Handles are opaque
// host file handles, not SdFat objects.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  int read(void* dst, const size_t bytes) { return handle_ ? g_pdf_host->file_read(handle_, dst, bytes) : -1; }
  size_t write(const void* src, const size_t bytes) {
    const int n = handle_ ? g_pdf_host->file_write(handle_, src, bytes) : -1;
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  bool seek64(const uint64_t offset) { return handle_ && g_pdf_host->file_seek(handle_, offset) != 0; }
  uint64_t fileSize64() const { return handle_ ? g_pdf_host->file_size(handle_) : 0; }
  bool isOpen() const { return handle_ != nullptr; }
  void close() {
    if (handle_) {
      g_pdf_host->file_close(handle_);
      handle_ = nullptr;
    }
  }

 private:
  void* handle_ = nullptr;
  friend struct PdfHostStorage;
};

struct PdfHostStorage {
  static PdfHostStorage& instance();
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.close();
    file.handle_ = g_pdf_host->file_open_read(path);
    return file.handle_ != nullptr;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.close();
    file.handle_ = g_pdf_host->file_open_write(path);
    return file.handle_ != nullptr;
  }
  bool remove(const char* path) { return g_pdf_host->file_remove(path); }
};

#define Storage PdfHostStorage::instance()

#endif  // CROSSPOINT_PDF_MODULE
