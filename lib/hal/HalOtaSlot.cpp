#include "HalOtaSlot.h"

#include <Logging.h>
#include <esp_cpu.h>
#include <esp_memory_utils.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace {
const esp_partition_t* asPartition(const void* partition) { return static_cast<const esp_partition_t*>(partition); }

// Any internal-flash read/write/erase disables the caches for its duration. A
// task whose stack lives in PSRAM cannot survive that: ESP-IDF asserts in
// spi_flash_disable_interrupts_caches_and_other_cpu(). The render task's stack
// is in PSRAM (the CFF interpreter needs 32 KB), and font prewarming reaches
// this slot through SdCardFontCache. Refuse here so the caller takes its
// existing fallback (fonts read from their SD source) instead of panicking.
// / 读/写/擦内部 flash 期间 cache 是关闭的；栈在 PSRAM 的任务此时不可运行，
// IDF 会直接断言。渲染任务的栈就在 PSRAM（CFF 需要 32 KB），字体 prewarm 会
// 经 SdCardFontCache 走到这里。这里拒绝，让调用方走既有回退（字体改读 SD）。
bool stackIsInPsram() { return esp_ptr_external_ram(esp_cpu_get_sp()); }

constexpr HalOtaSlot::RunningImageState classifyImageState(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_PENDING_VERIFY:
      return HalOtaSlot::RunningImageState::PendingVerify;
    case ESP_OTA_IMG_VALID:
      return HalOtaSlot::RunningImageState::Confirmed;
    case ESP_OTA_IMG_UNDEFINED:
      return HalOtaSlot::RunningImageState::Untracked;
    case ESP_OTA_IMG_NEW:
    case ESP_OTA_IMG_INVALID:
    case ESP_OTA_IMG_ABORTED:
      return HalOtaSlot::RunningImageState::Unsafe;
  }
  return HalOtaSlot::RunningImageState::Unsafe;
}

static_assert(classifyImageState(ESP_OTA_IMG_PENDING_VERIFY) == HalOtaSlot::RunningImageState::PendingVerify);
static_assert(classifyImageState(ESP_OTA_IMG_VALID) == HalOtaSlot::RunningImageState::Confirmed);
static_assert(classifyImageState(ESP_OTA_IMG_UNDEFINED) == HalOtaSlot::RunningImageState::Untracked);
static_assert(classifyImageState(ESP_OTA_IMG_NEW) == HalOtaSlot::RunningImageState::Unsafe);
static_assert(classifyImageState(ESP_OTA_IMG_INVALID) == HalOtaSlot::RunningImageState::Unsafe);
static_assert(classifyImageState(ESP_OTA_IMG_ABORTED) == HalOtaSlot::RunningImageState::Unsafe);
}  // namespace

#ifdef CONFIG_APP_ROLLBACK_ENABLE
extern "C" bool verifyRollbackLater() { return true; }
#endif

HalOtaSlot HalOtaSlot::inactive() {
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!partition || partition == running || partition->type != ESP_PARTITION_TYPE_APP ||
      partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
      partition->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
    return {};
  }
  return HalOtaSlot(partition, partition->size);
}

HalOtaSlot::RunningImageState HalOtaSlot::runningImageState() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_boot_partition() != running) return RunningImageState::Unsafe;

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t result = esp_ota_get_state_partition(running, &state);
  return result == ESP_OK ? classifyImageState(state) : RunningImageState::Unsafe;
}

bool HalOtaSlot::confirmRunningImage() {
  switch (runningImageState()) {
    case RunningImageState::Confirmed:
    case RunningImageState::Untracked:
      return true;
    case RunningImageState::PendingVerify:
      if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        LOG_ERR("OTA", "Failed to confirm running OTA image");
        return false;
      }
      return runningImageState() == RunningImageState::Confirmed;
    case RunningImageState::Unsafe:
      return false;
  }
  return false;
}

bool HalOtaSlot::safeForScratchWrite() const {
  if (!valid()) return false;

  switch (runningImageState()) {
    case RunningImageState::Confirmed:
    case RunningImageState::Untracked:
      return true;
    case RunningImageState::PendingVerify:
    case RunningImageState::Unsafe:
      return false;
  }
  return false;
}

bool HalOtaSlot::read(size_t offset, void* data, size_t length) const {
  if (!contains(offset, length)) return false;
  if (stackIsInPsram()) {
    LOG_ERR("OTA", "flash read refused: current task stack is in PSRAM (%u bytes)", static_cast<unsigned>(length));
    return false;
  }
  return esp_partition_read(asPartition(partition_), offset, data, length) == ESP_OK;
}

bool HalOtaSlot::erase(size_t offset, size_t length) const {
  if (!contains(offset, length) || offset % ERASE_SIZE != 0 || length % ERASE_SIZE != 0) return false;
  if (stackIsInPsram()) {
    LOG_ERR("OTA", "flash erase refused: current task stack is in PSRAM");
    return false;
  }
  return esp_partition_erase_range(asPartition(partition_), offset, length) == ESP_OK;
}

bool HalOtaSlot::write(size_t offset, const void* data, size_t length) const {
  if (!contains(offset, length)) return false;
  if (stackIsInPsram()) {
    LOG_ERR("OTA", "flash write refused: current task stack is in PSRAM (%u bytes)", static_cast<unsigned>(length));
    return false;
  }
  return esp_partition_write(asPartition(partition_), offset, data, length) == ESP_OK;
}

bool HalOtaSlot::contains(size_t offset, size_t length) const {
  return valid() && offset <= size_ && length <= size_ - offset;
}
