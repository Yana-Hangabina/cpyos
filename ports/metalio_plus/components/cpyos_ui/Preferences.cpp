#include "Preferences.h"

#include <nvs.h>
#include <nvs_flash.h>

namespace {
nvs_handle_t toHandle(uint32_t handle) { return static_cast<nvs_handle_t>(handle); }
}  // namespace

bool Preferences::begin(const char *name, bool readOnly) {
    end();
    if (nvs_flash_init() != ESP_OK) {
        return false;
    }
    nvs_handle_t handle = 0;
    const esp_err_t err =
        nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    handle_ = static_cast<uint32_t>(handle);
    open_ = true;
    return true;
}

void Preferences::end() {
    if (open_) {
        nvs_close(toHandle(handle_));
    }
    open_ = false;
    handle_ = 0;
}

bool Preferences::clear() {
    if (!open_) return false;
    return nvs_erase_all(toHandle(handle_)) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK;
}

bool Preferences::isKey(const char *key) {
    if (!open_) return false;
    size_t size = 0;
    return nvs_get_blob(toHandle(handle_), key, nullptr, &size) == ESP_OK ||
           nvs_get_u8(toHandle(handle_), key, nullptr) == ESP_OK;
}

uint8_t Preferences::getUChar(const char *key, uint8_t defaultValue) {
    if (!open_) return defaultValue;
    uint8_t value = 0;
    return nvs_get_u8(toHandle(handle_), key, &value) == ESP_OK ? value : defaultValue;
}

size_t Preferences::putUChar(const char *key, uint8_t value) {
    if (!open_) return 0;
    return nvs_set_u8(toHandle(handle_), key, value) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK
               ? sizeof(value)
               : 0;
}

uint32_t Preferences::getUInt(const char *key, uint32_t defaultValue) {
    if (!open_) return defaultValue;
    uint32_t value = 0;
    return nvs_get_u32(toHandle(handle_), key, &value) == ESP_OK ? value : defaultValue;
}

size_t Preferences::putUInt(const char *key, uint32_t value) {
    if (!open_) return 0;
    return nvs_set_u32(toHandle(handle_), key, value) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK
               ? sizeof(value)
               : 0;
}

int64_t Preferences::getLong64(const char *key, int64_t defaultValue) {
    if (!open_) return defaultValue;
    int64_t value = 0;
    return nvs_get_i64(toHandle(handle_), key, &value) == ESP_OK ? value : defaultValue;
}

size_t Preferences::putLong64(const char *key, int64_t value) {
    if (!open_) return 0;
    return nvs_set_i64(toHandle(handle_), key, value) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK
               ? sizeof(value)
               : 0;
}

bool Preferences::getBool(const char *key, bool defaultValue) {
    return getUChar(key, defaultValue ? 1 : 0) != 0;
}

size_t Preferences::putBool(const char *key, bool value) { return putUChar(key, value ? 1 : 0); }

String Preferences::getString(const char *key, const String &defaultValue) {
    if (!open_) return defaultValue;
    size_t size = 0;
    if (nvs_get_str(toHandle(handle_), key, nullptr, &size) != ESP_OK || size == 0) {
        return defaultValue;
    }
    char *buffer = static_cast<char *>(malloc(size));
    if (buffer == nullptr) {
        return defaultValue;
    }
    String value = defaultValue;
    if (nvs_get_str(toHandle(handle_), key, buffer, &size) == ESP_OK) {
        value = buffer;
    }
    free(buffer);
    return value;
}

size_t Preferences::putString(const char *key, const String &value) {
    return putString(key, value.c_str());
}

size_t Preferences::putString(const char *key, const char *value) {
    if (!open_ || value == nullptr) return 0;
    return nvs_set_str(toHandle(handle_), key, value) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK
               ? String(value).length()
               : 0;
}

size_t Preferences::remove(const char *key) {
    if (!open_) return 0;
    return nvs_erase_key(toHandle(handle_), key) == ESP_OK && nvs_commit(toHandle(handle_)) == ESP_OK ? 1 : 0;
}
