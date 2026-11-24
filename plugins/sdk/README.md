# CPYOS native plugin SDK

A plugin is a relocatable 32-bit ELF with `app_main(int argc, char** argv)`.
`argv[0]` is a `const CpyosPluginApi*`; `argv[1]` is the installed id. The ABI is
plain C and append-only; see `src/plugins/cpyos_plugin_api.h` in the firmware
tree. Do not import libc, C++ runtime or host symbols: every service goes through
the table.

## Package

```
/.crosspoint/plugins/<id>/manifest.json
/.crosspoint/plugins/<id>/plugin.elf
```

See `plugins/native/README.md` for the manifest schema. A catalog row can also
carry `sha256`; the host verifies it while scanning and after download.

## Start from the template

```bash
cp -R plugins/sdk/template plugins/native/myplugin
# edit plugins/native/myplugin/main/plugin.c and manifest.json
```

Read Pico (Xtensa) build:

```bash
bash plugins/sdk/build_xtensa.sh plugins/native/myplugin/main/plugin.c \
  plugins/native/myplugin/dist/plugin.elf
cp plugins/native/myplugin/manifest-xtensa.json plugins/native/myplugin/dist/manifest.json
```

The script uses `-ffreestanding -fno-builtin` (otherwise libc `memset` becomes
an undefined symbol) and `-mno-text-section-literals`; both are required for the
Read Pico loader.

Metalio E-INK4 Plus (RISC-V) build uses the same `project_elf()` recipe:

```bash
cp -R plugins/sdk/template plugins/native/myplugin
cd plugins/native/myplugin
. /path/to/esp-idf-6.1/export.sh
idf.py --preview set-target esp32s31   # first time only
idf.py --preview build elf
# build/myplugin.app.elf is the package payload
```

The template's `manifest.json` is for the Plus (`arch: riscv32`); use a
separate `manifest-xtensa.json` with `arch: xtensa` for Read Pico.

## Runtime limits

The host gives one plugin a 32 KiB PSRAM stack on Read Pico (24 KiB internal on
the Plus) and a 2 MiB PSRAM allocation budget with at most 32 live allocations.
`api->data_read()` / `data_write()` persist small files in
`/.crosspoint/plugin_data/<id>/` (64 KiB per write). Drawing is clipped by the
renderer; input is a bounded queue. Back is the host's exit gesture;
`api->request_exit()` is for a plugin's own quit action. The host reclaims
allocations when the plugin returns.
