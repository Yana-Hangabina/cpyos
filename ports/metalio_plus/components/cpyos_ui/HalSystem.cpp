#include "HalSystem.h"

#include <Arduino.h>

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace HalSystem {

void begin() {}
void restart() { esp_restart(); }
void checkPanic() {}
void clearPanic() {}
std::string getPanicInfo(bool /*full*/) { return {}; }

bool isRebootFromPanic() { return esp_reset_reason() == ESP_RST_PANIC; }

bool isRebootDeliberate() {
    switch (esp_reset_reason()) {
        case ESP_RST_USB:
        case ESP_RST_JTAG:
        case ESP_RST_SW:
        case ESP_RST_EXT:
        case ESP_RST_DEEPSLEEP:
            return true;
        default:
            return false;
    }
}

const char* getDeviceModel() { return "Metalio E-INK4 Plus"; }

const char* getChipModel() {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        default: return "ESP32-S31";
    }
}

bool getDeviceId(DeviceId& out) {
    out.fill(0);
    return esp_read_mac(out.data(), ESP_MAC_WIFI_STA) == ESP_OK;
}

bool getWifiStationMac(DeviceId& out) { return getDeviceId(out); }

bool getChipTemperatureCelsius(float& /*out*/) {
    // No temperature sensor is wired on the Plus BSP.
    return false;
}

uint64_t getUptimeSeconds() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
}

HeapInfo getHeapInfo() {
    HeapInfo info{};
    info.freeBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    info.totalBytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_8BIT));
    info.largestFreeBlockBytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    info.freePsramBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    info.totalPsramBytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    return info;
}

}  // namespace HalSystem
