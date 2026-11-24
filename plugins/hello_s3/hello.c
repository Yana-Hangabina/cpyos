// Phase 0 native plugin for readpico (Xtensa).
//
// No libc/PLT imports: the host passes a small C API table in argv[0]. This
// keeps the plugin's dynamic relocation surface empty and is the ABI shape the
// runtime will use for real plugins. Normal mode draws and holds the frame for
// the host; fast mode returns immediately for load/unload soak.

typedef struct CpyosApi {
    void (*draw)(const char* text);
} CpyosApi;

int app_main(int argc, char** argv) {
    const CpyosApi* api = (const CpyosApi*)argv[0];
    const int fast = argc > 1 && argv[1] != 0 && argv[1][0] == 'f';
    if (!fast && api != 0 && api->draw != 0) {
        api->draw("Hello from native ELF plugin");
    }
    return 0;
}
