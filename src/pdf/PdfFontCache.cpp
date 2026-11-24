#if defined(CROSSPOINT_PDF_XIP)

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <new>

// C ABI used by the MuPDF module (pdf-font.c) to move embedded font programs
// out of PSRAM.  The module decompresses a font stream in 4 KiB chunks, writes
// it here, then opens the file for FreeType through a custom FT_Stream; the
// rendered font bytes never have to stay in RAM.
namespace {

constexpr const char* kTag = "PDFF";
constexpr const char* kDir = "/.crosspoint/pdf_font_cache";
constexpr const char* kTemp = "/.crosspoint/pdf_font_cache/.part";
constexpr int kSlots = 8;

HalFile* g_write = nullptr;
struct Slot {
  HalFile* file = nullptr;
};
Slot g_slots[kSlots];

int findSlot() {
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slots[i].file) return i;
  }
  return -1;
}

}  // namespace

extern "C" int cp_pdf_font_cache_begin(void) {
  if (g_write) return 0;
  Storage.mkdir(kDir, true);
  auto* file = new (std::nothrow) HalFile();
  if (!file) return 0;
  if (!Storage.openFileForWrite(kTag, kTemp, *file)) {
    delete file;
    return 0;
  }
  g_write = file;
  return 1;
}

extern "C" int cp_pdf_font_cache_write(const void* data, unsigned len) {
  if (!g_write || !data) return 0;
  HalStorage::StorageLock lock;
  return g_write->write(data, len) == len ? 1 : 0;
}

extern "C" int cp_pdf_font_cache_commit(const char* name, char* out, unsigned cap) {
  if (!g_write || !name || !out || cap == 0) return 0;
  g_write->close();
  delete g_write;
  g_write = nullptr;
  HalStorage::StorageLock lock;
  char finalPath[160];
  std::snprintf(finalPath, sizeof(finalPath), "%s/%s.fnt", kDir, name);
  if (!Storage.exists(finalPath)) {
    if (!Storage.replaceFile(kTemp, finalPath)) {
      Storage.remove(kTemp);
      return 0;
    }
  } else {
    Storage.remove(kTemp);
  }
  std::snprintf(out, cap, "%s", finalPath);
  return 1;
}

extern "C" void cp_pdf_font_cache_abort(void) {
  if (g_write) {
    g_write->close();
    delete g_write;
    g_write = nullptr;
  }
  Storage.remove(kTemp);
}

extern "C" int cp_pdf_font_cache_open(const char* path) {
  if (!path || !*path) return -1;
  const int slot = findSlot();
  if (slot < 0) return -1;
  auto* file = new (std::nothrow) HalFile();
  if (!file) return -1;
  if (!Storage.openFileForRead(kTag, path, *file)) {
    delete file;
    return -1;
  }
  g_slots[slot].file = file;
  return slot;
}

extern "C" long cp_pdf_font_cache_size(int handle) {
  if (handle < 0 || handle >= kSlots || !g_slots[handle].file) return 0;
  return static_cast<long>(g_slots[handle].file->fileSize64());
}

extern "C" long cp_pdf_font_cache_read(int handle, unsigned long offset, void* buffer, unsigned long len) {
  if (handle < 0 || handle >= kSlots || !g_slots[handle].file || !buffer || len == 0) return 0;
  HalFile* file = g_slots[handle].file;
  HalStorage::StorageLock lock;
  if (!file->seek64(static_cast<uint64_t>(offset))) return 0;
  const int n = file->read(buffer, static_cast<size_t>(len));
  return n > 0 ? static_cast<long>(n) : 0;
}

extern "C" void cp_pdf_font_cache_close(int handle) {
  if (handle < 0 || handle >= kSlots) return;
  if (g_slots[handle].file) {
    HalStorage::StorageLock lock;
    g_slots[handle].file->close();
    delete g_slots[handle].file;
    g_slots[handle].file = nullptr;
  }
}

#endif
