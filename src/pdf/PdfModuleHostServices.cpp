// App-side implementation of the engine host table.  One table serves both
// loaders (flash partition and SD), so the services the engine can reach are
// defined exactly once and cannot drift between targets.

#if defined(CROSSPOINT_PDF_XIP)

#include "PdfModuleRuntime.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <new>

#include "CmapStore.h"

// Implemented in PdfFontCache.cpp; declared here so the table can point at them
// without pulling that translation unit's storage details into every user.
extern "C" int cp_pdf_font_cache_begin(void);
extern "C" int cp_pdf_font_cache_write(const void* data, unsigned len);
extern "C" int cp_pdf_font_cache_commit(const char* name, char* out, unsigned cap);
extern "C" void cp_pdf_font_cache_abort(void);
extern "C" int cp_pdf_font_cache_open(const char* path);
extern "C" long cp_pdf_font_cache_size(int handle);
extern "C" long cp_pdf_font_cache_read(int handle, unsigned long offset, void* buffer, unsigned long len);
extern "C" void cp_pdf_font_cache_close(int handle);

namespace pdf_module {
namespace {

// The engine holds an opaque handle; the app owns the HalFile behind it.  At
// most a document stream, a BMP writer and the font cache are open at once.
struct HostFile {
  HalFile file;
};

void* hostOpenRead(const char* path) {
  if (!path) return nullptr;
  auto* h = new (std::nothrow) HostFile();
  if (!h) {
    LOG_ERR("PDFM", "host file handle allocation failed");
    return nullptr;
  }
  if (!Storage.openFileForRead("PDFM", path, h->file)) {
    delete h;
    return nullptr;
  }
  return h;
}

void* hostOpenWrite(const char* path) {
  if (!path) return nullptr;
  auto* h = new (std::nothrow) HostFile();
  if (!h) {
    LOG_ERR("PDFM", "host file handle allocation failed");
    return nullptr;
  }
  if (!Storage.openFileForWrite("PDFM", path, h->file)) {
    delete h;
    return nullptr;
  }
  return h;
}

int hostRead(void* handle, void* dst, const size_t bytes) {
  return handle ? static_cast<HostFile*>(handle)->file.read(dst, bytes) : -1;
}

int hostWrite(void* handle, const void* src, const size_t bytes) {
  return handle ? static_cast<int>(static_cast<HostFile*>(handle)->file.write(src, bytes)) : -1;
}

int hostSeek(void* handle, const uint64_t offset) {
  return handle && static_cast<HostFile*>(handle)->file.seek64(offset) ? 1 : 0;
}

uint64_t hostSize(void* handle) {
  return handle ? static_cast<HostFile*>(handle)->file.fileSize64() : 0;
}

void hostClose(void* handle) { delete static_cast<HostFile*>(handle); }

bool hostRemove(const char* path) { return path && Storage.remove(path); }

void* hostAlignedAlloc(const size_t alignment, const size_t bytes, const uint32_t caps) {
  return heap_caps_aligned_alloc(alignment ? alignment : 8, bytes, caps);
}

const PdfModuleHost g_host = {
    .abi = kPdfModuleHostApi,
    .structSize = sizeof(PdfModuleHost),
    .heap_alloc = [](const size_t bytes, const uint32_t caps) { return heap_caps_malloc(bytes, caps); },
    .heap_aligned_alloc = hostAlignedAlloc,
    .heap_realloc = [](void* ptr, const size_t bytes, const uint32_t caps) {
      return heap_caps_realloc(ptr, bytes, caps);
    },
    .heap_free = [](void* ptr) { heap_caps_free(ptr); },
    .heap_allocated_size = [](const void* ptr) { return heap_caps_get_allocated_size(const_cast<void*>(ptr)); },
    .heap_free_size = [](const uint32_t caps) { return heap_caps_get_free_size(caps); },
    .heap_largest_free_block = [](const uint32_t caps) { return heap_caps_get_largest_free_block(caps); },
    .heap_total_size = [](const uint32_t caps) { return heap_caps_get_total_size(caps); },
    .log = logPrintf,
    .millis = millis,
    .delay_ms = [](const uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); },
    .backtrace = [](const int depth) { esp_backtrace_print(depth); },
    .file_open_read = hostOpenRead,
    .file_open_write = hostOpenWrite,
    .file_read = hostRead,
    .file_write = hostWrite,
    .file_seek = hostSeek,
    .file_size = hostSize,
    .file_close = hostClose,
    .file_remove = hostRemove,
    .cmap_load = crosspoint_cmap_load,
    .font_cache_begin = cp_pdf_font_cache_begin,
    .font_cache_write = cp_pdf_font_cache_write,
    .font_cache_commit = cp_pdf_font_cache_commit,
    .font_cache_abort = cp_pdf_font_cache_abort,
    .font_cache_open = cp_pdf_font_cache_open,
    .font_cache_size = cp_pdf_font_cache_size,
    .font_cache_read = cp_pdf_font_cache_read,
    .font_cache_close = cp_pdf_font_cache_close,
};

}  // namespace

const PdfModuleHost* hostTable() { return &g_host; }

}  // namespace pdf_module

#endif  // CROSSPOINT_PDF_XIP
