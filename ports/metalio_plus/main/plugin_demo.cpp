// Native ELF plugin Phase 0 host.
//
// This is deliberately a bench command, not the product plugin manager: it
// proves that an ELF built for ESP32-S31 can be loaded from the SD card into
// PSRAM, relocated, executed, and unloaded while the shell stays alive.
//
// It also exposes the first pieces of the host C ABI. The plugin resolves
// `cpyos_plugin_draw_text` through esp_elf_register_symbol(); no C++ symbol,
// object layout, STL type or core singleton crosses the boundary.

#include "plugin_demo.h"

#include <esp_elf.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activities/ActivityManager.h"
#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "activities/RenderLock.h"
#include "fontIds.h"

extern GfxRenderer renderer;
extern ActivityManager activityManager;

extern "C" void cpyos_plugin_draw_text(const char* text);

namespace {

static const esp_elfsym s_plugin_symbols[] = {
    ESP_ELFSYM_EXPORT(cpyos_plugin_draw_text),
    ESP_ELFSYM_END,
};

struct PluginFile {
    FILE* fp = nullptr;
    size_t size = 0;
};

size_t pluginFileSize(void* ctx) {
    return static_cast<PluginFile*>(ctx)->size;
}

int pluginFileRead(void* ctx, size_t off, void* buf, size_t len) {
    auto* file = static_cast<PluginFile*>(ctx);
    if (file->fp == nullptr) return -EIO;
    if (fseek(file->fp, static_cast<long>(off), SEEK_SET) != 0) return -EIO;
    const size_t got = fread(buf, 1, len, file->fp);
    return got == len ? 0 : -EIO;
}

void pluginFileClose(void* ctx) {
    auto* file = static_cast<PluginFile*>(ctx);
    if (file->fp != nullptr) {
        fclose(file->fp);
        file->fp = nullptr;
    }
}

void logMemory(const char* stage) {
    printf("plugin: %s psram free=%u largest=%u | int free=%u largest=%u\n", stage,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void runPlugin(const char* path, int loops, bool fast) {
    static bool symbolsRegistered = false;
    if (!symbolsRegistered) {
        // The table itself lives in this firmware; only its address is handed to
        // the loader. The plugin resolves the C name at relocation time.
        if (esp_elf_register_symbol(s_plugin_symbols) != 0) {
            printf("plugin: symbol table registration failed\n");
            return;
        }
        symbolsRegistered = true;
    }

    for (int i = 0; i < loops; ++i) {
        FILE* fp = fopen(path, "rb");
        if (fp == nullptr) {
            printf("plugin: open failed: %s\n", path);
            return;
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            printf("plugin: seek failed: %s\n", path);
            return;
        }
        const long size = ftell(fp);
        if (size <= 0) {
            fclose(fp);
            printf("plugin: empty or unseekable: %s\n", path);
            return;
        }
        rewind(fp);

        PluginFile file{fp, static_cast<size_t>(size)};
        const elf_source_t source = {
            pluginFileSize,
            pluginFileRead,
            pluginFileClose,
            &file,
        };

        esp_elf_t elf{};
        if (esp_elf_init(&elf) != 0) {
            fclose(fp);
            printf("plugin: esp_elf_init failed\n");
            return;
        }

        logMemory("before");
        const int64_t loadStart = esp_timer_get_time();
        const int relocate = esp_elf_relocate_source(&elf, &source);
        const int64_t loadEnd = esp_timer_get_time();
        if (relocate != 0) {
            printf("plugin: relocate failed rc=%d (%s)\n", relocate, path);
            esp_elf_deinit(&elf);
            return;
        }
        logMemory("loaded");

        char slow[] = "slow";
        char quick[] = "fast";
        char* argv[] = {slow, fast ? quick : slow, nullptr};
        const int64_t runStart = esp_timer_get_time();
        const int result = esp_elf_request(&elf, 0, fast ? 2 : 1, argv);
        const int64_t runEnd = esp_timer_get_time();

        printf("plugin: run=%d size=%u load=%lldus exec=%lldus loop=%d/%d\n", result,
               static_cast<unsigned>(size), static_cast<long long>(loadEnd - loadStart),
               static_cast<long long>(runEnd - runStart), i + 1, loops);

        esp_elf_deinit(&elf);
        logMemory("after");
        activityManager.requestUpdate(true);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

struct PluginJob {
    char path[192];
    int loops;
    bool fast;
};

void pluginTask(void* arg) {
    auto* job = static_cast<PluginJob*>(arg);
    runPlugin(job->path, job->loops, job->fast);
    free(job);
    vTaskDelete(nullptr);
}

void schedule(const char* path, int loops, bool fast) {
    auto* job = static_cast<PluginJob*>(malloc(sizeof(PluginJob)));
    if (job == nullptr) {
        printf("plugin: no memory for job\n");
        return;
    }
    snprintf(job->path, sizeof(job->path), "%s", path);
    job->loops = loops;
    job->fast = fast;
    // 16 KiB comes from internal RAM: the plugin task must be able to run
    // loader/core code that touches flash/NVS/cache, and the plugin body lives
    // in PSRAM (the loader's own allocations are PSRAM).
    if (xTaskCreate(pluginTask, "plugin-demo", 32768, job, 5, nullptr) != pdPASS) {
        free(job);
        printf("plugin: task creation failed\n");
        return;
    }
    printf("plugin: scheduled %s x%d (%s)\n", path, loops, fast ? "fast" : "normal");
}

}  // namespace

// The first host ABI entry point. It is intentionally tiny: the plugin task
// takes the render lock, draws a full frame and pushes it. Later phases will
// replace this with a capability-based API struct.
extern "C" void cpyos_plugin_draw_text(const char* text) {
    if (text == nullptr) return;
    RenderLock lock;
    renderer.clearScreen(0xFF);
    renderer.drawText(UI_12_FONT_ID, 24, 40, text, true);
    renderer.drawText(UI_12_FONT_ID, 24, 90, "native ELF plugin on ESP32-S31", true);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void consolePlugin(const char* args) {
    char path[192] = "/sdcard/plugins/hello.app.elf";
    if (args != nullptr && *args != '\0') {
        if (sscanf(args, "%191s", path) != 1) {
            printf("plugin: bad path\n");
            return;
        }
    }
    schedule(path, 1, false);
}

void consolePluginLoop(const char* args) {
    int loops = 10;
    char path[192] = "/sdcard/plugins/hello.app.elf";
    if (args != nullptr && *args != '\0') {
        const int parsed = sscanf(args, "%d %191s", &loops, path);
        if (parsed < 1 || loops < 1) {
            printf("usage: pluginloop <n> [path]\n");
            return;
        }
        if (loops > 200) loops = 200;
    }
    schedule(path, loops, true);
}
