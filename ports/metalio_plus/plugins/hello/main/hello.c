// Phase 0 native plugin: proves the host C ABI, PSRAM execution and unload.
// printf/sleep resolve through the host libc symbol table; the draw callback is
// registered by the shell (see main/plugin_demo.cpp).

#include <stdio.h>
#include <unistd.h>

extern void cpyos_plugin_draw_text(const char* text) __attribute__((visibility("default")));

int app_main(int argc, char** argv) {
    const int fast = argc > 1 && argv[1] != NULL && argv[1][0] == 'f';
    printf("hello plugin: entry fast=%d\n", fast);
    if (!fast) {
        cpyos_plugin_draw_text("Hello from native ELF plugin");
        sleep(2);
    }
    printf("hello plugin: exit\n");
    return 0;
}
