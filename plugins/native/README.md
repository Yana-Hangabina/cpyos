# Native plugin packages

An out-of-tree plugin is a directory on the SD card:

```
/.crosspoint/plugins/<id>/manifest.json
/.crosspoint/plugins/<id>/plugin.elf
```

`manifest.json` (see `src/activities/apps/plugins` and
`docs/engineering/native-plugins.md`):

```json
{
  "id": "2048",
  "name": "2048",
  "version": "1.0.0",
  "api": 1,
  "arch": "riscv32",
  "entry": "app_main",
  "elf": "plugin.elf",
  "description": "One line for the picker",
  "author": "CPYOS"
}
```

`id` must match its directory, use only `[A-Za-z0-9._-]`, and `arch` must name
the running SoC (`riscv32` on the Plus, `xtensa` on Read Pico). `api` is
`CPYOS_PLUGIN_API_VERSION` from `src/plugins/cpyos_plugin_api.h`.

Plugins are loaded by `esp_elf` from the SD card into PSRAM, called through
`app_main(argc, argv)` with a `const CpyosPluginApi*` in `argv[0]`, and unloaded
on exit. There is no libc/PLT contract: draw, input, time, logging and PSRAM
allocation go through the API table. Back is reserved for the host; a plugin may
call `api->request_exit()` for its own quit action.

See `2048/` for the reference package and build script.

## Game packages (Phase 4)

`build_games.sh` builds every game for both targets:

| id | package |
|---|---|
| `2048` | 2048, persistent board/score |
| `sokoban` | three built-in Sokoban levels |
| `gomoku` | two-player 15x15 five-in-a-row |
| `minesweeper` | 9x9 mine field |
| `sudoku` | fixed 9x9 puzzle |
| `chinese-chess` | two-player Xiangqi with basic move rules |

These are the externalized, playable plugin versions; the in-app games remain
available in non-plugin builds as a rollback while parity work continues.
