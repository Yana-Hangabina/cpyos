// Module-only runtime glue: libc/new/STL entry points and the MuPDF asset hooks
// are implemented here on top of the host table, so the engine image has no
// undefined references into the app.  Never compiled into the firmware.

#if defined(CROSSPOINT_PDF_MODULE)

#include "PdfModuleHost.h"
#include "PdfModuleHostGlue.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

const PdfModuleHost* g_pdf_host = nullptr;

PdfHostStorage& PdfHostStorage::instance() {
  static PdfHostStorage storage;
  return storage;
}

// --- heap -------------------------------------------------------------------

extern "C" void* malloc(const size_t bytes) {
  return g_pdf_host ? g_pdf_host->heap_alloc(bytes, kPdfHostCapSpiram | kPdfHostCap8Bit) : nullptr;
}

extern "C" void* calloc(const size_t count, const size_t size) {
  const size_t total = count * size;
  void* p = malloc(total);
  if (p) {
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < total; ++i) b[i] = 0;
  }
  return p;
}

extern "C" void* realloc(void* ptr, const size_t bytes) {
  return g_pdf_host ? g_pdf_host->heap_realloc(ptr, bytes, kPdfHostCapSpiram | kPdfHostCap8Bit) : nullptr;
}

extern "C" void free(void* ptr) {
  if (g_pdf_host) g_pdf_host->heap_free(ptr);
}

extern "C" void* aligned_alloc(const size_t alignment, const size_t bytes) {
  return g_pdf_host ? g_pdf_host->heap_aligned_alloc(alignment, bytes, kPdfHostCapSpiram | kPdfHostCap8Bit)
                    : nullptr;
}

extern "C" int posix_memalign(void** out, const size_t alignment, const size_t bytes) {
  if (!out) return 22;  // EINVAL
  void* p = aligned_alloc(alignment, bytes);
  if (!p) return 12;  // ENOMEM
  *out = p;
  return 0;
}

// Unknown-but-pullable C hooks: the engine never opens stdio files itself (all
// I/O goes through the host), so make a call fail instead of dragging picolibc's
// sbrk-based allocator in.
extern "C" void* sbrk(const ptrdiff_t) { return reinterpret_cast<void*>(-1); }
extern "C" void* _sbrk(const ptrdiff_t) { return reinterpret_cast<void*>(-1); }

// --- C++ runtime ------------------------------------------------------------

void* operator new(const size_t bytes) {
  void* p = malloc(bytes ? bytes : 1);
  if (!p) std::abort();
  return p;
}
void* operator new[](const size_t bytes) { return operator new(bytes); }
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept { return malloc(bytes ? bytes : 1); }
void* operator new[](const size_t bytes, const std::nothrow_t&) noexcept { return malloc(bytes ? bytes : 1); }
void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, const size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, const size_t) noexcept { free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

namespace std {
// libstdc++ normally supplies these; the module links none of it.
const nothrow_t nothrow{};
[[noreturn]] void __throw_length_error(const char*) { abort(); }
[[noreturn]] void __throw_logic_error(const char*) { abort(); }
[[noreturn]] void __throw_bad_function_call() { abort(); }
[[noreturn]] void __throw_out_of_range(const char*) { abort(); }
[[noreturn]] void __throw_bad_alloc() { abort(); }
}  // namespace std

// --- MuPDF asset hooks (patched sources call these by name) -----------------

extern "C" const unsigned char* crosspoint_cmap_load(const char* name, size_t* size) {
  if (!g_pdf_host || !g_pdf_host->cmap_load) {
    if (size) *size = 0;
    return nullptr;
  }
  return g_pdf_host->cmap_load(name, size);
}

#define HOST_CALL(name, fail_value, ...) (g_pdf_host && g_pdf_host->name ? g_pdf_host->name(__VA_ARGS__) : (fail_value))

extern "C" int cp_pdf_font_cache_begin() { return HOST_CALL(font_cache_begin, -1); }
extern "C" int cp_pdf_font_cache_write(const void* data, unsigned len) {
  return HOST_CALL(font_cache_write, -1, data, len);
}
extern "C" int cp_pdf_font_cache_commit(const char* name, char* out, unsigned cap) {
  return HOST_CALL(font_cache_commit, -1, name, out, cap);
}
extern "C" void cp_pdf_font_cache_abort() {
  if (g_pdf_host && g_pdf_host->font_cache_abort) g_pdf_host->font_cache_abort();
}
extern "C" int cp_pdf_font_cache_open(const char* path) { return HOST_CALL(font_cache_open, -1, path); }
extern "C" long cp_pdf_font_cache_size(int handle) { return HOST_CALL(font_cache_size, -1, handle); }
extern "C" long cp_pdf_font_cache_read(int handle, unsigned long offset, void* buffer, unsigned long len) {
  return HOST_CALL(font_cache_read, -1, handle, offset, buffer, len);
}
extern "C" void cp_pdf_font_cache_close(int handle) {
  if (g_pdf_host && g_pdf_host->font_cache_close) g_pdf_host->font_cache_close(handle);
}

#endif  // CROSSPOINT_PDF_MODULE
