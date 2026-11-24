// Phase-0 probe for the SD code-paging plan. It does not render PDFs; it only
// proves whether ESP32-S3 can execute PSRAM-backed code pages through an MMU
// window and whether an ITLB-miss handler can remap a page on demand.
//
// Enable with -DCROSSPOINT_PAGING_PROBE. The serial entry point is CMD:PGPROBE.
#if defined(CROSSPOINT_PAGING_PROBE)

#include <Arduino.h>
#include <Logging.h>
#include <esp_cache.h>
#include <esp_mmu_map.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <hal/mmu_hal.h>
#include <soc/soc.h>
#include <xtensa_api.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kProbeVaddr2 = 0x43100000;  // never mapped: forces ITLB miss
constexpr uint32_t kFlashCodeVaddr = 0x43000000;
constexpr uint32_t kPsramDataVaddr = 0x3E000000;
constexpr uint32_t kProbeApp1Offset = 0x600000;  // test payload flashed at 0xC70000

// Xtensa little-endian machine code for: int probe_add(int x) { return x + 1; }
constexpr uint8_t kProbeCode[] = {0x36, 0x21, 0x00, 0x1b, 0x22, 0x1d, 0xf0};

uint8_t* gPage = nullptr;
uint32_t gPaddr = 0;
uint32_t gVaddr = 0;
uint32_t gPageSize = 0;
bool gHandlerInstalled = false;
volatile uint32_t gFaultCount = 0;

void IRAM_ATTR itlbMissHandler(XtExcFrame* frame) {
  ++gFaultCount;
  const uint32_t faultPc = frame->pc;
  LOG_ERR("PGPROBE", "ITLB miss pc=0x%08x", static_cast<unsigned>(faultPc));
  if (gPaddr == 0 || gVaddr == 0 || gPageSize == 0) return;

  const uint32_t pageBase = gVaddr & ~(gPageSize - 1);
  uint32_t mapped = 0;
  // ESP32-S3 has one MMU peripheral; instruction/data mappings share it.
  mmu_hal_map_region(0, MMU_TARGET_PSRAM0, pageBase, static_cast<uint32_t>(gPaddr), gPageSize, &mapped);
  esp_cache_msync(reinterpret_cast<void*>(pageBase), gPageSize,
                  ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  if (mapped < gPageSize) {
    LOG_ERR("PGPROBE", "remap failed: mapped=%u", static_cast<unsigned>(mapped));
  }
}

void runFlashXip() {
  const esp_partition_t* app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!app1) {
    LOG_ERR("PGPROBE", "flash xip: app1 partition missing");
    return;
  }
  uint8_t direct[8] = {};
  if (esp_partition_read(app1, kProbeApp1Offset, direct, sizeof(direct)) != ESP_OK) {
    LOG_ERR("PGPROBE", "flash xip: read app1 payload failed");
    return;
  }
  LOG_INF("PGPROBE", "flash xip direct=%02x%02x%02x%02x%02x%02x%02x%02x", direct[0], direct[1], direct[2],
          direct[3], direct[4], direct[5], direct[6], direct[7]);

  uint32_t mapped = 0;
  mmu_hal_map_region(0, MMU_TARGET_FLASH0, kFlashCodeVaddr, kProbeApp1Offset, gPageSize, &mapped);
  esp_cache_msync(reinterpret_cast<void*>(kFlashCodeVaddr), gPageSize,
                  ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  const auto* bytes = reinterpret_cast<const uint8_t*>(kFlashCodeVaddr);
  LOG_INF("PGPROBE", "flash xip mapped=%u bytes=%02x%02x%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(mapped),
          bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
  if (mapped >= sizeof(direct) && std::memcmp(bytes, direct, sizeof(direct)) == 0) {
    using ProbeFn = int (*)(int);
    auto fn = reinterpret_cast<ProbeFn>(kFlashCodeVaddr);
    LOG_INF("PGPROBE", "flash xip call=%d (expect 42)", fn(41));
  } else {
    mmu_hal_unmap_region(0, kFlashCodeVaddr, gPageSize);
    mmu_hal_map_region(0, MMU_TARGET_FLASH0, kFlashCodeVaddr, app1->address + kProbeApp1Offset, gPageSize, &mapped);
    esp_cache_msync(reinterpret_cast<void*>(kFlashCodeVaddr), gPageSize,
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    LOG_INF("PGPROBE", "flash xip absolute mapped=%u bytes=%02x%02x%02x%02x", static_cast<unsigned>(mapped),
            bytes[0], bytes[1], bytes[2], bytes[3]);
    if (mapped >= sizeof(direct) && std::memcmp(bytes, direct, sizeof(direct)) == 0) {
      using ProbeFn = int (*)(int);
      auto fn = reinterpret_cast<ProbeFn>(kFlashCodeVaddr);
      LOG_INF("PGPROBE", "flash xip absolute call=%d (expect 42)", fn(41));
    }
  }
}

void runPsramDataAlias() {
  uint8_t* page = static_cast<uint8_t*>(heap_caps_aligned_alloc(gPageSize, gPageSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!page) {
    LOG_ERR("PGPROBE", "psram data: allocation failed");
    return;
  }
  std::memset(page, 0, gPageSize);
  uint32_t paddr = 0;
  mmu_target_t target = MMU_TARGET_FLASH0;
  esp_err_t err = esp_mmu_vaddr_to_paddr(page, reinterpret_cast<esp_paddr_t*>(&paddr), &target);
  uint32_t mapped = 0;
  mmu_hal_map_region(0, MMU_TARGET_PSRAM0, kPsramDataVaddr, paddr, gPageSize, &mapped);
  auto* alias = reinterpret_cast<uint32_t*>(kPsramDataVaddr);
  alias[0] = 0x5044464d;
  alias[1] = 0x13572468;
  esp_cache_msync(reinterpret_cast<void*>(kPsramDataVaddr), 16, ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  const uint32_t heap0 = reinterpret_cast<uint32_t*>(page)[0];
  const uint32_t alias0 = alias[0];
  LOG_INF("PGPROBE", "psram data paddr=0x%08x target=%d err=%d mapped=%u heap=%08x alias=%08x",
          static_cast<unsigned>(paddr), static_cast<int>(target), static_cast<int>(err),
          static_cast<unsigned>(mapped), static_cast<unsigned>(heap0), static_cast<unsigned>(alias0));
  heap_caps_free(page);
}

}  // namespace

namespace pdf_paging_probe {

void run() {
  gPageSize = mmu_hal_pages_to_bytes(0, 1);
  gPage = static_cast<uint8_t*>(heap_caps_aligned_alloc(gPageSize, gPageSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (gPage == nullptr) {
    LOG_ERR("PGPROBE", "PSRAM page allocation failed");
    return;
  }
  std::memset(gPage, 0, gPageSize);
  std::memcpy(gPage, kProbeCode, sizeof(kProbeCode));
  esp_cache_msync(gPage, gPageSize, 0);

  // PSRAM external virtual space starts at SOC_DROM_LOW; the MMU paddr is
  // the offset from that base for a heap_caps_malloc(SPIRAM) block.
  mmu_target_t target = MMU_TARGET_FLASH0;
  esp_err_t perr = esp_mmu_vaddr_to_paddr(gPage, reinterpret_cast<esp_paddr_t*>(&gPaddr), &target);
  if (perr != ESP_OK) {
    gPaddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gPage) - SOC_DROM_LOW);
  }
  LOG_INF("PGPROBE", "page=%p paddr=0x%08x target=%d err=%d", gPage, static_cast<unsigned>(gPaddr),
          static_cast<int>(target), static_cast<int>(perr));
  void* mapped = nullptr;
  esp_err_t merr = esp_mmu_map(gPaddr, gPageSize, MMU_TARGET_PSRAM0,
                               static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ), 0, &mapped);
  LOG_INF("PGPROBE", "esp_mmu_map err=%d mapped=%p", static_cast<int>(merr), mapped);
  if (merr != ESP_OK || mapped == nullptr) return;
  gVaddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mapped));

  using AddFn = int (*)(int);
  auto add = reinterpret_cast<AddFn>(mapped);
  const int first = add(41);
  LOG_INF("PGPROBE", "exec mapped page=%p result=%d (expect 42)", mapped, first);

  // The never-mapped-page test is intentionally skipped here: it panics in
  // the cache/MMU hardware before any Xtensa exception handler is reached.
  // That result is recorded in docs/engineering/pdf-rendering.md; this build
  // now exercises the fixed-address flash-XIP and fixed PSRAM-data probes.
  LOG_INF("PGPROBE", "SUMMARY mapped=%u first=%d", gPageSize, first);
  runFlashXip();
  runPsramDataAlias();
}

}  // namespace pdf_paging_probe

#endif  // CROSSPOINT_PAGING_PROBE
