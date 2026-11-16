#include "plus_log_mirror.h"

#include <esp_log.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "HalStorage.h"
#include "PortRadioState.h"

namespace {

constexpr size_t kRingBytes = 16 * 1024;
constexpr size_t kChunk = 2048;
constexpr uint32_t kFlushPeriodMs = 3000;
constexpr const char* kLogPath = "/.crosspoint/logs/boot.log";
constexpr const char* kLogDir = "/.crosspoint/logs";
// Keep the file bounded; it is rewritten when it would grow past this.
constexpr size_t kMaxLogBytes = 192 * 1024;

char s_ring[kRingBytes];

// The mirrored file outlives the session, so anything that looks like a
// credential must be masked before it is written. The shared HTTP debug lines
// carry `token=`/`loginName=`/`loginPassword=` and cookie headers; a card copy
// of those is exactly what the security notes forbid.
void redactSecrets(char* line) {
    static const char* const kKeys[] = {"sid", "token", "loginname", "loginpass", "password",
                                        "checkdevicecode", "authkey", "cookie", "set-cookie",
                                        "authorization"};
    char* p = line;
    while (*p) {
        // find 'key=', case-insensitively, at a word boundary-ish position
        for (const char* key : kKeys) {
            const size_t kl = strlen(key);
            bool match = true;
            for (size_t i = 0; i < kl; ++i) {
                const char c = static_cast<char>(tolower(static_cast<unsigned char>(p[i])));
                if (c != key[i] || p[i] == '\0') {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            const char* after = p + kl;
            while (*after == ' ' || *after == '"' || *after == '\'') ++after;
            if (*after != '=') continue;
            char* v = const_cast<char*>(after) + 1;
            while (*v == ' ' || *v == '"' || *v == '\'') ++v;
            char* end = v;
            while (*end && *end != '&' && *end != ' ' && *end != '\t' && *end != '"' && *end != '\'' &&
                   *end != ';' && *end != '\r' && *end != '\n') {
                ++end;
            }
            for (char* q = v; q < end; ++q) *q = '*';
            p = end;
            break;
        }
        if (*p) ++p;
    }
}
size_t s_head = 0;  // write position
size_t s_used = 0;
SemaphoreHandle_t s_lock = nullptr;
vprintf_like_t s_original = nullptr;
size_t s_dirty = 0;
bool s_storageReady = false;

void ringAppend(const char* text, size_t len) {
    if (len >= kRingBytes) {
        text += len - kRingBytes;
        len = kRingBytes;
    }
    for (size_t i = 0; i < len; ++i) {
        s_ring[s_head] = text[i];
        s_head = (s_head + 1) % kRingBytes;
    }
    s_used = s_used + len > kRingBytes ? kRingBytes : s_used + len;
    s_dirty = s_dirty + len > kRingBytes ? kRingBytes : s_dirty + len;
}

// Copy out up to `max` bytes oldest-first and consume them.
size_t ringTake(char* out, size_t max) {
    const size_t n = s_used < max ? s_used : max;
    if (n == 0) return 0;
    size_t start = (s_head + kRingBytes - s_used) % kRingBytes;
    for (size_t i = 0; i < n; ++i) {
        out[i] = s_ring[(start + i) % kRingBytes];
    }
    s_used -= n;
    return n;
}

int mirrorVprintf(const char* fmt, va_list args) {
    char line[512];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) redactSecrets(line);
    if (n > 0 && s_lock != nullptr && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        ringAppend(line, static_cast<size_t>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1));
        xSemaphoreGive(s_lock);
    }
    return s_original != nullptr ? s_original(fmt, args) : vprintf(fmt, args);
}

void flushRing() {
    if (!s_storageReady) return;
    if (g_portRadioBusy) return;  // radio needs the internal DMA blocks
    if (s_lock == nullptr || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    const bool dirty = s_dirty > 0;
    if (dirty) s_dirty = 0;
    xSemaphoreGive(s_lock);
    if (!dirty) return;

    static char chunk[kChunk];
    for (;;) {
        size_t n = 0;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            n = ringTake(chunk, sizeof(chunk));
            xSemaphoreGive(s_lock);
        }
        if (n == 0) break;

        // Rotate when the file is already large; a fresh file keeps the head of
        // the new run instead of unbounded growth on a busy card.
        struct stat st{};
        const bool big = ::stat("/sdcard/.crosspoint/logs/boot.log", &st) == 0 &&
                         static_cast<size_t>(st.st_size) > kMaxLogBytes;
        if (big) {
            Storage.remove(kLogPath);
        }
        HalFile f = Storage.open(kLogPath, O_WRONLY | O_CREAT | O_APPEND);
        if (!f) return;
        f.write(reinterpret_cast<const uint8_t*>(chunk), n);
        f.flush();
        f.close();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void mirrorTask(void*) {
    Storage.ensureDirectoryExists("/.crosspoint");
    Storage.ensureDirectoryExists(kLogDir);
    s_storageReady = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kFlushPeriodMs));
        flushRing();
    }
}

}  // namespace

// Strong definition of the weak hook declared in Logging.cpp: every shared
// LOG_* line lands in the same ring as ESP_LOG, so the card copy is complete.
extern "C" void logMirrorAppend(const char* line, size_t len) {
    if (line == nullptr || len == 0 || s_lock == nullptr) return;
    char copy[512];
    size_t n = len < sizeof(copy) - 1 ? len : sizeof(copy) - 1;
    memcpy(copy, line, n);
    copy[n] = '\0';
    redactSecrets(copy);
    n = strlen(copy);
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        ringAppend(copy, n);
        ringAppend("\n", 1);
        xSemaphoreGive(s_lock);
    }
}

void plusLogMirrorStart() {
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) return;
    }
    if (s_original == nullptr) {
        s_original = esp_log_set_vprintf(mirrorVprintf);
    }
    static bool taskStarted = false;
    if (!taskStarted) {
        taskStarted = true;
        xTaskCreate(mirrorTask, "plus-logmirror", 3072, nullptr, 1, nullptr);
    }
}
