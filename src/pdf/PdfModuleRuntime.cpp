#include "PdfModuleRuntime.h"
#include "PdfModuleHeaderCheck.h"

// The flash-partition loader.  The S31 port keeps the same ABI but loads the
// image from SD into PSRAM (PdfModuleRuntimeSd.cpp); the guards are exclusive so
// exactly one translator unit defines the pdf_module API.
#if defined(CROSSPOINT_PDF_XIP) && !defined(CROSSPOINT_PDF_XIP_SD)

#include <HalStorage.h>
#include <Logging.h>
#include <StackSafety.h>
#include <esp_cache.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <esp_mmu_map.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <hal/cache_hal.h>
#include <hal/cache_ll.h>
#include <hal/mmu_hal.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace pdf_module {
namespace {

constexpr uint32_t kPage = 0x10000;
constexpr uint32_t kCodeReserve = 0x200000;  // fixed virtual window, not physical
constexpr uint32_t kDataReserve = 0x40000;

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint32_t alignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1u) & ~(alignment - 1u); }

// esptool writes the SHA-256 of the linked ELF at this offset of every app
// image (see the --elf-sha256-offset in the firmware.bin build rule).  It is
// build provenance: compatibility is decided by pdfModuleCompatible(), not by
// this digest.
constexpr uint32_t kElfSha256Offset = 0xb0;

bool readRunningDigest(uint8_t out[32]) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr || esp_partition_read(running, kElfSha256Offset, out, 32) != ESP_OK) return false;
  bool zero = true;
  for (int i = 0; i < 32; ++i) {
    if (out[i] != 0u) {
      zero = false;
      break;
    }
  }
  return !zero;
}

const esp_partition_t* g_part = nullptr;
Refusal g_refusal = Refusal::None;
uint32_t g_debugMappedCodeAddr = 0;
PdfModuleOps g_ops{};
bool g_loaded = false;
std::atomic<uint32_t> g_openDocuments{0};
uint8_t* g_data = nullptr;
uint32_t g_dataSize = 0;
uint32_t g_dataMapSize = 0;
uint32_t g_codeSize = 0;
uint32_t g_codeMapSize = 0;
uint32_t g_rodataMapSize = 0;

void clearState() {
  g_ops = {};
  // Keep the reason: clearState() runs on the failure paths that set it, and
  // the reader reads lastRefusal() only after ensureLoaded() returned false.
  if (g_refusal == Refusal::None) g_refusal = Refusal::BadHeader;
  g_loaded = false;
  g_data = nullptr;
  g_dataSize = 0;
  g_dataMapSize = 0;
  g_codeSize = 0;
  g_codeMapSize = 0;
  g_rodataMapSize = 0;
}

bool validHeader(const PdfModuleHeader& h) {
  if (h.headerBytes != kPdfModuleHeaderBytes || h.codeOffset != kPdfModuleHeaderBytes) return false;
  if (h.codeSize == 0 || h.codeSize > kCodeReserve || h.entryOffset >= h.codeSize) return false;
  if (h.rodataSize == 0 || h.rodataSize > kCodeReserve) return false;
  if (h.dataSize > kDataReserve || h.bssSize > kDataReserve || h.dataSize > kPdfModuleDataBssGap) return false;
  // Section ordering, entry bounds, compatibility class and "does it fit" come
  // from the SAME predicate the installer runs before it erases
  // (src/pdf/PdfModuleHeaderCheck.h).  At load time the file length IS the
  // partition length, hence both arguments.  Duplicating these rules here is how
  // two graders start disagreeing about the same image.
  const uint64_t span = static_cast<uint64_t>(g_part->size);
  const pdf_header::Status status = pdf_header::check(h, span, span);
  if (pdf_header::isCompatibilityStatus(status)) {
    // Intact image, different contract: name it so the reader can offer the
    // update path instead of "engine magic is wrong".
    LOG_ERR("PDFM", "engine is incompatible: %s", pdf_header::reason(status));
    g_refusal = Refusal::Incompatible;
    return false;
  }
  return status == pdf_header::Status::Ok;
}

void enableCacheBus(uint32_t vaddr, uint32_t size) {
  const cache_bus_mask_t mask = cache_ll_l1_get_bus(0, vaddr, size);
  cache_ll_l1_enable_bus(0, mask);
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
  cache_ll_l1_enable_bus(1, mask);
#endif
}

}  // namespace

bool ensureLoaded() {
  if (g_loaded) return true;

  // A crash in the XIP engine can leave the PSRAM init state inconsistent so
  // that the app's psramInit left no PSRAM heap.  Re-initialize here before
  // touching the MMU; if that still fails, refuse the engine instead of
  // letting the framebuffer/render task assert and reboot-loop.
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
    if (!psramInit()) {
      LOG_ERR("PDFM", "PSRAM init retry failed");
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
      LOG_ERR("PDFM", "no PSRAM heap; refusing the XIP engine (power-cycle required)");
      g_refusal = Refusal::NoPsram;
      return false;
    }
  }

  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
                                    kPdfModulePartition);
  if (!g_part) {
    g_refusal = Refusal::NoPartition;
    LOG_ERR("PDFM", "module partition `%s` missing", kPdfModulePartition);
    return false;
  }

  PdfModuleHeader h{};
  if (esp_partition_read(g_part, 0, &h, sizeof(h)) != ESP_OK || !validHeader(h)) {
    LOG_ERR("PDFM", "module header rejected");
    return false;
  }

  // Code/rodata are mapped directly from flash.  The image contains .text at
  // kPdfModuleCodeVaddr and .rodata at a fixed offset, so the linker's absolute
  // calls and literals remain valid without a runtime relocation pass.
  g_codeSize = h.codeSize;
  // Use the high-level mapper: it performs the MMU write, cache-bus enable and
  // instruction/data cache invalidation in one operation.  The module is
  // linked at kPdfModuleCodeVaddr, so the allocator must choose that exact
  // window; the app image layout was rebased once (0x42D20000 -> 0x42D30000)
  // when the UI code grew across a 64 KiB boundary.
  g_codeMapSize = alignUp(g_codeSize, kPage);
  void* mappedCode = nullptr;
  const esp_err_t codeMapErr = esp_mmu_map(g_part->address + h.codeOffset, g_codeMapSize, MMU_TARGET_FLASH0,
                                           static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ), 0,
                                           &mappedCode);
  g_debugMappedCodeAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mappedCode));
  if (codeMapErr != ESP_OK || mappedCode != reinterpret_cast<void*>(kPdfModuleCodeVaddr)) {
    g_refusal = Refusal::MapFailed;
    LOG_ERR("PDFM", "code map err=%d at %p (need 0x%08x)", static_cast<int>(codeMapErr), mappedCode,
            static_cast<unsigned>(kPdfModuleCodeVaddr));
    clearState();
    return false;
  }
  LOG_INF("PDFM", "code mapped at %p", mappedCode);

  // .literal stays in .text near code (l32r range), but string/table data is
  // .rodata and must be in a data-readable DROM window.
  const uint32_t rodataMapSize = alignUp(h.rodataSize, kPage);
  g_rodataMapSize = rodataMapSize;
  uint32_t mappedRodata = 0;
  mmu_hal_map_region(0, MMU_TARGET_FLASH0, kPdfModuleRodataVaddr, g_part->address + h.rodataOffset,
                     rodataMapSize, &mappedRodata);
  enableCacheBus(kPdfModuleRodataVaddr, rodataMapSize);
  cache_hal_invalidate_addr(kPdfModuleRodataVaddr, rodataMapSize);
  esp_cache_msync(reinterpret_cast<void*>(kPdfModuleRodataVaddr), rodataMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  LOG_INF("PDFM", "rodata mapped raw at %p (%u bytes)", reinterpret_cast<void*>(kPdfModuleRodataVaddr),
          static_cast<unsigned>(rodataMapSize));

  // Mutable globals are linked at a fixed DROM alias.  They live in PSRAM;
  // map the physical pages there so the module's absolute data addresses work.
  // .bss is linked at a fixed offset beyond .data (see build_pdf_module.py);
  // the two are not adjacent in the module ELF.
  g_dataSize = kPdfModuleDataBssGap + h.bssSize;
  g_dataMapSize = alignUp(g_dataSize ? g_dataSize : 1, kPage);
  g_data = static_cast<uint8_t*>(heap_caps_aligned_alloc(kPage, g_dataMapSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g_data) {
    LOG_ERR("PDFM", "module data allocation failed (%u)", static_cast<unsigned>(g_dataMapSize));
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }
  uint32_t paddr = 0;
  mmu_target_t target = MMU_TARGET_FLASH0;
  if (esp_mmu_vaddr_to_paddr(g_data, reinterpret_cast<esp_paddr_t*>(&paddr), &target) != ESP_OK || (paddr % kPage) != 0) {
    LOG_ERR("PDFM", "module data address is not MMU-mappable");
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }
  // Map the module's window with the raw MMU call instead of esp_mmu_map:
  // the allocator behind the latter hands out the first free page range, so a
  // module linked for one fixed address stops loading as soon as the app image
  // (and therefore the ext_ram layout) shifts.  0x3D800000 is past the app's
  // static ext_ram and the PSRAM heap, and the raw mapper accepts it.
  uint32_t mappedData = 0;
  mmu_hal_map_region(0, MMU_TARGET_PSRAM0, kPdfModuleDataVaddr, paddr, g_dataMapSize, &mappedData);
  if (mappedData < g_dataMapSize) {
    LOG_ERR("PDFM", "data map failed (%u of %u)", static_cast<unsigned>(mappedData),
            static_cast<unsigned>(g_dataMapSize));
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }
  enableCacheBus(kPdfModuleDataVaddr, g_dataMapSize);
  cache_hal_invalidate_addr(kPdfModuleDataVaddr, g_dataMapSize);
  uint32_t dataCheckPaddr = 0;
  mmu_target_t dataTarget = MMU_TARGET_FLASH0;
  if (!mmu_hal_vaddr_to_paddr(0, kPdfModuleDataVaddr, &dataCheckPaddr, &dataTarget)) {
    LOG_ERR("PDFM", "data mapping not readable");
    mmu_hal_unmap_region(0, kPdfModuleDataVaddr, g_dataMapSize);
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }

  auto* dataAlias = reinterpret_cast<uint8_t*>(kPdfModuleDataVaddr);
  uint32_t aliasPaddr = 0;
  mmu_target_t aliasTarget = MMU_TARGET_FLASH0;
  mmu_hal_vaddr_to_paddr(0, kPdfModuleDataVaddr, &aliasPaddr, &aliasTarget);
  LOG_INF("PDFM", "data alias=%p heap=%p paddr=0x%08x aliasPaddr=0x%08x target=%d", static_cast<void*>(dataAlias),
          static_cast<void*>(g_data), static_cast<unsigned>(paddr), static_cast<unsigned>(aliasPaddr),
          static_cast<int>(aliasTarget));
  // Read through a temporary internal buffer and copy through the CPU cache;
  // a direct flash-to-PSRAM-alias read may bypass the cache the module sees.
  uint8_t dataScratch[512];
  size_t dataRead = 0;
  bool dataOk = true;
  while (dataRead < h.dataSize) {
    const size_t chunk = std::min(sizeof(dataScratch), static_cast<size_t>(h.dataSize) - dataRead);
    if (esp_partition_read(g_part, h.dataOffset + dataRead, dataScratch, chunk) != ESP_OK) {
      dataOk = false;
      break;
    }
    std::memcpy(dataAlias + dataRead, dataScratch, chunk);
    dataRead += chunk;
  }
  if (!dataOk) {
    LOG_ERR("PDFM", "module data read failed");
    mmu_hal_unmap_region(0, kPdfModuleDataVaddr, g_dataMapSize);
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }
  std::memset(dataAlias + kPdfModuleDataBssGap, 0, h.bssSize);
  // Make the flash-loaded .data and zeroed .bss visible through the alias
  // before the module's first static access.
  esp_cache_msync(reinterpret_cast<void*>(kPdfModuleDataVaddr), g_dataMapSize,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  // Read the zeroed span back through the *heap* pointer, not the alias.  The
  // memset above went through the alias, so re-reading the alias would prove
  // nothing; seeing the zeros through g_data is what shows the alias and this
  // allocation are the same physical pages.  If that ever stops being true the
  // module would run on uninitialized statics -- which is exactly how the
  // 2026-11-19 crash started (MuPDF dereferenced 0xcdcd out of unzeroed
  // .ext_ram.bss), so refuse loudly instead of entering the engine.
  bool bssClean = true;
  uint32_t dirtyAt = 0;
  for (uint32_t off = 0; off + sizeof(uint32_t) <= h.bssSize; off += sizeof(uint32_t)) {
    const uint32_t word = *reinterpret_cast<const uint32_t*>(g_data + kPdfModuleDataBssGap + off);
    if (word != 0u) {
      bssClean = false;
      dirtyAt = off;
      break;
    }
  }
  if (!bssClean) {
    LOG_ERR("PDFM", "module .bss read-back is not zero at +%u; alias and heap differ",
            static_cast<unsigned>(dirtyAt));
    g_refusal = Refusal::MapFailed;
    mmu_hal_unmap_region(0, kPdfModuleDataVaddr, g_dataMapSize);
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
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
    mmu_hal_unmap_region(0, kPdfModuleDataVaddr, g_dataMapSize);
    heap_caps_free(g_data);
    mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
    mmu_hal_unmap_region(0, kPdfModuleCodeVaddr, g_codeMapSize);
    clearState();
    return false;
  }

  g_loaded = true;
  g_refusal = Refusal::None;
  LOG_INF("PDFM", "engine loaded: code=%u data=%u bss=%u", static_cast<unsigned>(h.codeSize),
          static_cast<unsigned>(h.dataSize), static_cast<unsigned>(h.bssSize));
  return true;
}

static bool gUnloadPending = false;

void scheduleUnload() { gUnloadPending = true; }

bool flushScheduledUnload(const bool readerIdle) {
  if (!gUnloadPending) return false;
  if (!readerIdle) return false;  // the reader may be loading its engine right now
  if (openDocuments() != 0) {
    // The reader came back before the loop ran: the engine is in use again.
    gUnloadPending = false;
    return false;
  }
  // Same rule as onExit's own guard: never touch the MMU from a PSRAM stack.
  if (!memory::internalFlashAllowed(fault::Code::PdfRuntimeMapRefused)) return false;
  gUnloadPending = false;
  if (!g_loaded) return false;  // nothing mapped: no unmap log should claim one
  unload();
  return true;
}

void unload() {
  if (!g_loaded) return;
  // The caller must close all documents first.
  mmu_hal_unmap_region(0, kPdfModuleDataVaddr, g_dataMapSize);
  if (g_data) heap_caps_free(g_data);
  mmu_hal_unmap_region(0, kPdfModuleRodataVaddr, g_rodataMapSize);
  esp_mmu_unmap(reinterpret_cast<void*>(kPdfModuleCodeVaddr));
  clearState();
}

// Counted on every PdfDocument handle transition, so the install path can ask
// instead of trusting its callers to remember.  Documents are opened on the UI
// task and on the PDF worker, hence relaxed atomics rather than a plain int.
void noteDocumentOpened() { g_openDocuments.fetch_add(1, std::memory_order_relaxed); }

void noteDocumentClosed() {
  const uint32_t before = g_openDocuments.load(std::memory_order_relaxed);
  if (before == 0) {
    // Never seen, but loud: an unbalanced close here would underflow the
    // counter and permanently block engine installs.
    LOG_ERR("PDFM", "document close without a matching open; counter left at 0");
    return;
  }
  g_openDocuments.fetch_sub(1, std::memory_order_relaxed);
}

uint32_t openDocuments() { return g_openDocuments.load(std::memory_order_relaxed); }

uint32_t loadedAbi() { return g_loaded ? g_ops.abi : 0; }
uint32_t debugMappedCodeAddr() { return g_debugMappedCodeAddr; }

void debugLogImageHeader() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), nullptr);
  if (part == nullptr) {
    LOG_INF("PDFM", "image: no pdfmod partition");
    return;
  }
  PdfModuleHeader h{};
  if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK || h.magic != kPdfModuleMagic) {
    LOG_INF("PDFM", "image: no header at %08x (needs install)", static_cast<unsigned>(part->address));
    return;
  }
  const auto status = pdf_header::check(h, part->size, part->size);
  LOG_INF("PDFM", "image: v%u abi=%u host=%u.%u layout=0x%08x rev=%u -> %s", static_cast<unsigned>(h.version),
          static_cast<unsigned>(h.abi), static_cast<unsigned>(h.hostApi >> 16),
          static_cast<unsigned>(h.hostApi & 0xFFFFu), static_cast<unsigned>(h.layoutKey),
          static_cast<unsigned>(h.engineRev), pdf_header::reason(status));
  LOG_INF("PDFM", "image: compatible=%d", pdf_header::isCompatibilityStatus(status) ? 0 : (status == pdf_header::Status::Ok));
}


const PdfModuleOps* ops() { return g_loaded ? &g_ops : nullptr; }
bool isLoaded() { return false; }

Refusal lastRefusal() { return g_refusal; }

namespace {

// Reads only the header of an SD engine image and reports how it relates to the
// running app, without touching the partition.  Shared by the pre-flight check
// (so the UI can explain a mismatch before erasing) and installFromSdFile().
InstallStatus inspectSdHeader(const char* path, const esp_partition_t* part, size_t& bytesOut,
                              PdfModuleHeader& hOut) {
  if (!path || !*path) return InstallStatus::NotFound;
  HalFile file;
  if (!Storage.openFileForRead("PDFM", path, file)) return InstallStatus::NotFound;
  const size_t bytes = file.fileSize();
  if (bytes <= kPdfModuleHeaderBytes || (part && bytes > part->size)) {
    file.close();
    return InstallStatus::BadHeader;
  }
  PdfModuleHeader h{};
  if (file.read(&h, sizeof(h)) != static_cast<int>(sizeof(h))) {
    file.close();
    return InstallStatus::BadHeader;
  }
  file.close();
  // Every geometry rule, BEFORE the erase below: a truncated image with an intact header
  // used to pass inspection here, wipe the one working engine, and only then fail the
  // loader's CRC check -- and this board has no BOOT/RESET button to recover with.
  const uint64_t partBytes = part ? static_cast<uint64_t>(part->size) : 0;
  const pdf_header::Status geometry = pdf_header::check(h, bytes, partBytes);
  if (geometry != pdf_header::Status::Ok) {
    LOG_ERR("PDFM", "engine image refused before erase: %s", pdf_header::reason(geometry));
    return pdf_header::isCompatibilityStatus(geometry) ? InstallStatus::Mismatch : InstallStatus::BadHeader;
  }
  bytesOut = bytes;
  hOut = h;
  return InstallStatus::Ok;
}

}  // namespace

InstallStatus checkSdImage(const char* path) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), kPdfModulePartition);
  size_t bytes = 0;
  PdfModuleHeader h{};
  return inspectSdHeader(path, part, bytes, h);
}

InstallStatus installFromSdFile(const char* path) {
  if (!path || !*path) return InstallStatus::NotFound;
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), kPdfModulePartition);
  if (!part) return InstallStatus::IoError;

  size_t bytes = 0;
  PdfModuleHeader h{};
  const InstallStatus head = inspectSdHeader(path, part, bytes, h);
  if (head != InstallStatus::Ok) return head;
  // A compatible image is refused before the erase if the header lied about its
  // class; inspectSdHeader() already ran the same predicate, so reaching here
  // means the image can run on this app.

  // Erasing implies unmapping, and MuPDF keeps pointers into that window in
  // every live document.  Ask the document count instead of trusting callers to
  // close first: this is the one refusal that turns a crash into an error code.
  if (const uint32_t open = openDocuments()) {
    LOG_ERR("PDFM", "%u document(s) still hold the engine; refusing to install", static_cast<unsigned>(open));
    return InstallStatus::Busy;
  }

  unload();
  if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) return InstallStatus::IoError;

  HalFile file;
  if (!Storage.openFileForRead("PDFM", path, file)) return InstallStatus::NotFound;
  // Skip the header read: rewind to 0 and copy the whole image, header included.
  uint8_t buffer[4096];
  size_t offset = 0;
  while (offset < bytes) {
    const size_t want = std::min(sizeof(buffer), bytes - offset);
    const int got = file.read(buffer, want);
    if (got != static_cast<int>(want) || esp_partition_write(part, offset, buffer, want) != ESP_OK) {
      file.close();
      return InstallStatus::IoError;
    }
    offset += want;
  }
  file.close();
  LOG_INF("PDFM", "module installed: %u bytes", static_cast<unsigned>(bytes));
  return InstallStatus::Ok;
}

const char* defaultSdImagePath() { return "/pdfmod.bin"; }

bool runningBuildId(uint8_t out[32]) { return readRunningDigest(out); }

}  // namespace pdf_module

#endif
