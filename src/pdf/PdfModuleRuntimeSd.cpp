// SD-backed PDF engine loader for the S31 (Metalio E-INK4 Plus) port.
//
// The Read Pico engine is executed in place from the dedicated `pdfmod` flash
// partition.  The Plus has no such partition (16 MB are fully allocated and
// both app slots are in use), but it has ~12 MB of free PSRAM and the S31 can
// execute code from external RAM through the MMU (the native game plugins are
// loaded exactly that way).  This loader reads the same module image from
// `/.crosspoint/pdf/pdfmod.bin`, copies it into PSRAM pages and maps them at
// the fixed virtual addresses the module was linked for.  Everything above it
// (PdfDocumentXip, the reader, the ops table) is unchanged.
//
// The app/engine pairing uses the version string instead of the app image's
// ELF-SHA field: the Plus flash script never writes that field (esptool is not
// given --elf-sha256-offset), and a stale engine is caught by the ABI/geometry
// checks plus the version stamp.

#include "PdfModuleRuntime.h"
#include "PdfModuleHeaderCheck.h"

#if defined(CROSSPOINT_PDF_XIP_SD)

#include <HalStorage.h>
#include <Logging.h>
#include <StackSafety.h>
#include <esp_cache.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_mmu_map.h>
#include <hal/cache_hal.h>
#include <hal/cache_ll.h>
#include <hal/mmu_hal.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace pdf_module {
namespace {

constexpr uint32_t kPage = 0x10000;
// S31 has two MMU tables: flash is id 0, PSRAM is id 1 (hal/esp32s31/mmu_ll.h).
// Programming the flash table with a PSRAM physical address made the mapped
// window read through the linear PSRAM path instead, which MSPI rejects as an
// out-of-range address.
constexpr uint32_t kPsramMmuId = 1;
constexpr uint32_t kCodeReserve = 0x200000;
constexpr uint32_t kDataReserve = 0x40000;
constexpr const char* kModulePath = "/.crosspoint/pdf/pdfmod.bin";
// Bootstrap path: the file-transfer page cannot write dot directories, so a
// card-side upload of the engine lands at the root.  The loader falls back to
// it only when the managed copy is absent (validated identically).
constexpr const char* kModulePathRoot = "/pdfmod.bin";

Refusal g_refusal = Refusal::None;
PdfModuleOps g_ops{};
bool g_loaded = false;
std::atomic<uint32_t> g_openDocuments{0};
uint8_t* g_codeMem = nullptr;
uint8_t* g_rodataMem = nullptr;
uint8_t* g_data = nullptr;
uint32_t g_codeSize = 0;
uint32_t g_codeMapSize = 0;
uint32_t g_rodataMapSize = 0;
uint32_t g_dataSize = 0;
uint32_t g_dataMapSize = 0;

uint32_t alignUp(const uint32_t value, const uint32_t alignment) { return (value + alignment - 1u) & ~(alignment - 1u); }

// esptool patches every flash-built app image with the SHA-256 of the linked
// ELF at offset 0xb0 (elf2image does this when the ELF has a `.flash.appdesc`
// segment; the Plus image carries it too -- verified on the 2026-11-23 build).
// That makes the pairing rule identical to the flash-partition loader instead
// of a version string that does not change across rebuilds.
constexpr uint32_t kElfSha256Offset = 0xb0;

bool readRunningDigest(uint8_t out[32]) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr || esp_partition_read(running, kElfSha256Offset, out, 32) != ESP_OK) return false;
  for (int i = 0; i < 32; ++i) {
    if (out[i] != 0u) return true;
  }
  return false;
}

void clearState() {
  g_ops = {};
  if (g_refusal == Refusal::None) g_refusal = Refusal::BadHeader;
  g_loaded = false;
  g_codeSize = 0;
  g_codeMapSize = 0;
  g_rodataMapSize = 0;
  g_dataSize = 0;
  g_dataMapSize = 0;
}

bool validHeader(const PdfModuleHeader& h, const uint64_t fileSize) {
  if (h.headerBytes != kPdfModuleHeaderBytes || h.codeOffset != kPdfModuleHeaderBytes) return false;
  if (h.codeSize == 0 || h.codeSize > kCodeReserve || h.entryOffset >= h.codeSize) return false;
  if (h.rodataSize == 0 || h.rodataSize > kCodeReserve) return false;
  if (h.dataSize > kDataReserve || h.bssSize > kDataReserve || h.dataSize > kPdfModuleDataBssGap) return false;
  const pdf_header::Status status = pdf_header::check(h, fileSize, fileSize);
  if (pdf_header::isCompatibilityStatus(status)) {
    LOG_ERR("PDFM", "engine is incompatible: %s", pdf_header::reason(status));
    g_refusal = Refusal::Incompatible;
    return false;
  }
  return status == pdf_header::Status::Ok;
}

void enableCacheBus(const uint32_t vaddr, const uint32_t size) {
  const cache_bus_mask_t mask = cache_ll_l1_get_bus(0, vaddr, size);
  cache_ll_l1_enable_bus(0, mask);
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
  cache_ll_l1_enable_bus(1, mask);
#endif
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// Reads a module section and, when `wantCrc` is non-null, verifies it against
// the header's CRC.  The image is user-writable (SD card), so a torn copy must
// be refused instead of executed; the header's CRC fields were previously
// written but never checked on this path.
bool readSpan(HalFile& file, const uint32_t offset, const uint32_t length, uint8_t* dst,
              const uint32_t wantCrc, const char* what) {
  if (length == 0) return true;
  if (!file.seek(offset)) return false;
  uint32_t crc = 0xffffffffu;
  size_t done = 0;
  while (done < length) {
    const int got = file.read(dst + done, length - done);
    if (got <= 0) return false;
    crc = crc32Update(crc, dst + done, static_cast<size_t>(got));
    done += static_cast<size_t>(got);
  }
  crc = ~crc;
  if (crc != wantCrc) {
    LOG_ERR("PDFM", "%s CRC mismatch (image 0x%08x, expected 0x%08x)", what, static_cast<unsigned>(crc),
            static_cast<unsigned>(wantCrc));
    return false;
  }
  return true;
}

// Maps `mapSize` bytes of fresh PSRAM at `vaddr`.  Returns the heap pointer
// (owned by the caller until unmap) or nullptr.  The mapping and the heap
// allocation are two views of the same physical pages; writes go through the
// mapped alias so the module's absolute addresses see them.
bool mapWindow(const uint32_t vaddr, const uint32_t mapSize, uint8_t** mem) {
  *mem = static_cast<uint8_t*>(heap_caps_aligned_alloc(kPage, mapSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (*mem == nullptr) return false;
  std::memset(*mem, 0, mapSize);
  uint32_t paddr = 0;
  mmu_target_t target = MMU_TARGET_FLASH0;
  if (esp_mmu_vaddr_to_paddr(*mem, reinterpret_cast<esp_paddr_t*>(&paddr), &target) != ESP_OK ||
      (paddr % kPage) != 0) {
    heap_caps_free(*mem);
    *mem = nullptr;
    return false;
  }
  // Refuse to overwrite an existing mapping: a fixed virtual window is only
  // safe when nothing else owns it.  Every page must be unmapped first.
  for (uint32_t off = 0; off < mapSize; off += kPage) {
    uint32_t existing = 0;
    mmu_target_t existingTarget = MMU_TARGET_FLASH0;
    if (mmu_hal_vaddr_to_paddr(kPsramMmuId, vaddr + off, &existing, &existingTarget)) {
      LOG_ERR("PDFM", "virtual window 0x%08x is already mapped; refusing", static_cast<unsigned>(vaddr + off));
      heap_caps_free(*mem);
      *mem = nullptr;
      return false;
    }
  }
  uint32_t mapped = 0;
  mmu_hal_map_region(kPsramMmuId, MMU_TARGET_PSRAM0, vaddr, paddr, mapSize, &mapped);
  if (mapped < mapSize) {
    heap_caps_free(*mem);
    *mem = nullptr;
    return false;
  }
  enableCacheBus(vaddr, mapSize);
  cache_hal_invalidate_addr(vaddr, mapSize);
  return true;
}

void unmapWindow(const uint32_t vaddr, const uint32_t mapSize, uint8_t*& mem) {
  if (mapSize != 0) mmu_hal_unmap_region(kPsramMmuId, vaddr, mapSize);
  if (mem != nullptr) heap_caps_free(mem);
  mem = nullptr;
}

bool loadImage(HalFile& file, const PdfModuleHeader& h) {
  g_codeSize = h.codeSize;
  g_codeMapSize = alignUp(h.codeSize, kPage);
  if (!mapWindow(kPdfModuleCodeVaddr, g_codeMapSize, &g_codeMem)) {
    LOG_ERR("PDFM", "code PSRAM/mapping failed (%u bytes)", static_cast<unsigned>(g_codeMapSize));
    return false;
  }
  auto* codeAlias = reinterpret_cast<uint8_t*>(kPdfModuleCodeVaddr);
  if (!readSpan(file, h.codeOffset, h.codeSize, codeAlias, h.codeCrc32, "code")) {
    LOG_ERR("PDFM", "code read failed");
    return false;
  }
  // The bytes were just written through the alias with the data cache, so
  // they must be written back as DATA first, and the instruction cache then
  // invalidated.  A single C2M|INST msync left some code lines unflushed and
  // the first jump into an unfetched function trapped as an illegal
  // instruction (floorf, observed on the first render).
  esp_cache_msync(codeAlias, g_codeMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  esp_cache_msync(codeAlias, g_codeMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  cache_hal_invalidate_addr(kPdfModuleCodeVaddr, g_codeMapSize);
  LOG_INF("PDFM", "code mapped at %p (%u bytes)", static_cast<void*>(codeAlias), static_cast<unsigned>(h.codeSize));

  g_rodataMapSize = alignUp(h.rodataSize, kPage);
  if (!mapWindow(kPdfModuleRodataVaddr, g_rodataMapSize, &g_rodataMem)) {
    LOG_ERR("PDFM", "rodata PSRAM/mapping failed");
    return false;
  }
  auto* rodataAlias = reinterpret_cast<uint8_t*>(kPdfModuleRodataVaddr);
  if (!readSpan(file, h.rodataOffset, h.rodataSize, rodataAlias, h.rodataCrc32, "rodata")) {
    LOG_ERR("PDFM", "rodata read failed");
    return false;
  }
  esp_cache_msync(rodataAlias, g_rodataMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  cache_hal_invalidate_addr(kPdfModuleRodataVaddr, g_rodataMapSize);
  LOG_INF("PDFM", "rodata mapped at %p (%u bytes)", static_cast<void*>(rodataAlias),
          static_cast<unsigned>(h.rodataSize));

  // Mutable globals: .data at kPdfModuleDataVaddr, .bss at +gap (see the
  // module linker script).  Same shape as the flash loader so the module's
  // absolute addresses stay valid.
  g_dataSize = kPdfModuleDataBssGap + h.bssSize;
  g_dataMapSize = alignUp(g_dataSize ? g_dataSize : 1, kPage);
  if (!mapWindow(kPdfModuleDataVaddr, g_dataMapSize, &g_data)) {
    LOG_ERR("PDFM", "data PSRAM/mapping failed");
    return false;
  }
  auto* dataAlias = reinterpret_cast<uint8_t*>(kPdfModuleDataVaddr);
  if (!readSpan(file, h.dataOffset, h.dataSize, dataAlias, h.dataCrc32, "data")) {
    LOG_ERR("PDFM", "module data read failed");
    return false;
  }
  std::memset(dataAlias + kPdfModuleDataBssGap, 0, h.bssSize);
  esp_cache_msync(dataAlias, g_dataMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

  bool bssClean = true;
  for (uint32_t off = 0; off + sizeof(uint32_t) <= h.bssSize; off += sizeof(uint32_t)) {
    if (*reinterpret_cast<const uint32_t*>(g_data + kPdfModuleDataBssGap + off) != 0u) {
      bssClean = false;
      break;
    }
  }
  if (!bssClean) {
    LOG_ERR("PDFM", "module .bss read-back is not zero; alias and heap differ");
    g_refusal = Refusal::MapFailed;
    return false;
  }

  g_ops.structSize = sizeof(PdfModuleOps);
  using Entry = int (*)(const PdfModuleHost*, PdfModuleOps*);
  auto entry = reinterpret_cast<Entry>(kPdfModuleCodeVaddr + h.entryOffset);
  LOG_INF("PDFM", "calling entry=%p ops=%p", reinterpret_cast<void*>(entry), static_cast<void*>(&g_ops));
  if (entry(hostTable(), &g_ops) != 0 || g_ops.abi != kPdfModuleAbiVersion || !g_ops.open || !g_ops.close ||
      !g_ops.needs_password || !g_ops.wrong_password || !g_ops.page_count || !g_ops.render_bmp ||
      !g_ops.extract_text || !g_ops.outline_count || !g_ops.outline_at) {
    LOG_ERR("PDFM", "module entry rejected (abi=%u)", static_cast<unsigned>(g_ops.abi));
    return false;
  }
  g_loaded = true;
  g_refusal = Refusal::None;
  LOG_INF("PDFM", "engine loaded from SD: code=%u data=%u bss=%u", static_cast<unsigned>(h.codeSize),
          static_cast<unsigned>(h.dataSize), static_cast<unsigned>(h.bssSize));
  return true;
}

bool ensureLoadedFromSd() {
  if (g_loaded) return true;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
    g_refusal = Refusal::NoPsram;
    LOG_ERR("PDFM", "no PSRAM heap; refusing the PDF engine");
    return false;
  }
  HalFile file;
  const char* source = kModulePath;
  if (!Storage.openFileForRead("PDFM", source, file)) {
    source = kModulePathRoot;
    if (!Storage.openFileForRead("PDFM", source, file)) {
      g_refusal = Refusal::NoPartition;  // "engine not installed" on this port
      LOG_ERR("PDFM", "engine image missing: %s (and %s)", kModulePath, kModulePathRoot);
      return false;
    }
    LOG_INF("PDFM", "using bootstrap engine at %s", source);
  }
  const uint64_t bytes = file.fileSize64();
  PdfModuleHeader h{};
  if (file.read(&h, sizeof(h)) != static_cast<int>(sizeof(h)) || !validHeader(h, bytes)) {
    LOG_ERR("PDFM", "module header rejected (%llu bytes)", static_cast<unsigned long long>(bytes));
    file.close();
    clearState();
    return false;
  }
  const bool ok = loadImage(file, h);
  file.close();
  if (!ok) {
    unmapWindow(kPdfModuleDataVaddr, g_dataMapSize, g_data);
    unmapWindow(kPdfModuleRodataVaddr, g_rodataMapSize, g_rodataMem);
    unmapWindow(kPdfModuleCodeVaddr, g_codeMapSize, g_codeMem);
    clearState();
    return false;
  }
  return true;
}

}  // namespace

bool ensureLoaded() { return ensureLoadedFromSd(); }

static bool gUnloadPending = false;

void scheduleUnload() { gUnloadPending = true; }

bool flushScheduledUnload(const bool readerIdle) {
  if (!gUnloadPending) return false;
  if (!readerIdle) return false;
  if (openDocuments() != 0) {
    gUnloadPending = false;
    return false;
  }
  if (!memory::internalFlashAllowed(fault::Code::PdfRuntimeSdRefused)) return false;
  gUnloadPending = false;
  if (!g_loaded) return false;
  unload();
  return true;
}

void unload() {
  if (!g_loaded) return;
  unmapWindow(kPdfModuleDataVaddr, g_dataMapSize, g_data);
  unmapWindow(kPdfModuleRodataVaddr, g_rodataMapSize, g_rodataMem);
  unmapWindow(kPdfModuleCodeVaddr, g_codeMapSize, g_codeMem);
  clearState();
}

void noteDocumentOpened() { g_openDocuments.fetch_add(1, std::memory_order_relaxed); }

void noteDocumentClosed() {
  const uint32_t before = g_openDocuments.load(std::memory_order_relaxed);
  if (before == 0) {
    LOG_ERR("PDFM", "document close without a matching open; counter left at 0");
    return;
  }
  g_openDocuments.fetch_sub(1, std::memory_order_relaxed);
}

uint32_t openDocuments() { return g_openDocuments.load(std::memory_order_relaxed); }
uint32_t loadedAbi() { return g_loaded ? g_ops.abi : 0; }
uint32_t debugMappedCodeAddr() { return 0; }
const PdfModuleOps* ops() { return g_loaded ? &g_ops : nullptr; }
bool isLoaded() { return g_loaded; }
Refusal lastRefusal() { return g_refusal; }

const char* defaultSdImagePath() { return kModulePathRoot; }

bool runningBuildId(uint8_t out[32]) {
  if (out == nullptr) return false;
  std::memset(out, 0, 32);
  return readRunningDigest(out);
}

InstallStatus checkSdImage(const char* path) {
  if (path == nullptr || *path == '\0') return InstallStatus::NotFound;
  HalFile file;
  if (!Storage.openFileForRead("PDFM", path, file)) return InstallStatus::NotFound;
  const uint64_t bytes = file.fileSize64();
  PdfModuleHeader h{};
  if (file.read(&h, sizeof(h)) != static_cast<int>(sizeof(h))) {
    file.close();
    return InstallStatus::BadHeader;
  }
  file.close();
  const pdf_header::Status status = pdf_header::check(h, bytes, bytes);
  if (status != pdf_header::Status::Ok) {
    return pdf_header::isCompatibilityStatus(status) ? InstallStatus::Mismatch : InstallStatus::BadHeader;
  }
  return InstallStatus::Ok;
}

InstallStatus installFromSdFile(const char* path) {
  if (path == nullptr || *path == '\0') return InstallStatus::NotFound;
  if (std::strcmp(path, kModulePath) == 0) return checkSdImage(path);
  const InstallStatus head = checkSdImage(path);
  if (head != InstallStatus::Ok) return head;
  if (openDocuments() != 0) return InstallStatus::Busy;

  // Copy to a sibling .part and swap it in; a failed update keeps the old
  // engine.  The engine is loaded lazily, so no mapping needs tearing down.
  std::string tmp = std::string(kModulePath) + ".part";
  HalFile src;
  if (!Storage.openFileForRead("PDFM", path, src)) return InstallStatus::NotFound;
  HalFile dst;
  if (!Storage.ensureDirectoryExists("/.crosspoint/pdf") ||
      !Storage.openFileForWrite("PDFM", tmp, dst)) {
    src.close();
    return InstallStatus::IoError;
  }
  uint8_t buffer[4096];
  bool ok = true;
  int got = 0;
  while ((got = src.read(buffer, sizeof(buffer))) > 0) {
    if (dst.write(buffer, static_cast<size_t>(got)) != got) {
      ok = false;
      break;
    }
  }
  src.close();
  dst.close();
  bool replaced = ok && Storage.replaceFile(tmp.c_str(), kModulePath);
  if (!replaced) {
    // Observed on the Plus: the remove+rename pair can fail right after a large
    // write (the second install over an existing engine returned IoError while
    // the first one succeeded).  Never leave an update half-applied: fall back
    // to an in-place overwrite, which the loader's CRC/size/tag checks still
    // validate on the next load.
    LOG_ERR("PDFM", "atomic engine replace failed; overwriting in place");
    Storage.remove(tmp.c_str());
    HalFile src2;
    HalFile out;
    if (!Storage.openFileForRead("PDFM", path, src2) || !Storage.openFileForWrite("PDFM", kModulePath, out)) {
      if (src2) src2.close();
      return InstallStatus::IoError;
    }
    uint8_t buffer2[4096];
    bool copied = true;
    int n2 = 0;
    while ((n2 = src2.read(buffer2, sizeof(buffer2))) > 0) {
      if (out.write(buffer2, static_cast<size_t>(n2)) != n2) {
        copied = false;
        break;
      }
    }
    src2.close();
    out.close();
    if (!copied) return InstallStatus::IoError;
  }
  // Do NOT unload here: on S31 an unmap invalidates caches that live PSRAM
  // state (framebuffer, page cache) is still using, and the same experiment
  // aborted the reader on exit.  The new image is used the next time the engine
  // is mapped (typically the next boot); the current session keeps running the
  // engine it already validated.
  LOG_INF("PDFM", "engine installed from %s; mapping kept until the next load", path);
  return InstallStatus::Ok;
}

}  // namespace pdf_module

#endif
