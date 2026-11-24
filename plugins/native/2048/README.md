# Native plugin pilot: 2048

This is the Phase 1 reference package. It is an out-of-tree ELF for the
ESP32-S31 (RISC-V), linked by `esp_elf` and loaded from
`/.crosspoint/plugins/2048/`.

```
/.crosspoint/plugins/2048/manifest.json
/.crosspoint/plugins/2048/plugin.elf
```

## Build

The build needs the ESP-IDF 6.1 checkout used by the Plus port:

```bash
export CPYOS_IDF_PATH=/Users/hongbin/Documents/projects/cpyos/.scratch/esp-idf-6.1
bash plugins/native/2048/build.sh
```

Output: `plugins/native/2048/dist/plugin.elf` plus a copy of `manifest.json`.
The loader requires the ELF to say `arch: riscv32` and `api: 1`.

## ABI

Only `src/plugins/cpyos_plugin_api.h` may be used. There is no libc, no C++
runtime and no symbol lookup at run time: the host passes a `CpyosPluginApi*`
in `argv[0]`, and every drawing/input/logging service goes through it. Back is
reserved for the host (cooperative exit); `api->request_exit()` is available
for a plugin's own quit action.

## Install

Copy the two files to the SD card (file transfer, USB drive, or the bench
serial `putfile` command). The plugin list rescans on entry and after the
`Retry` row is activated.
