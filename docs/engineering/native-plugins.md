# Native ELF plugins (Phase 1)

Status: **e4plus (ESP32-S31) implemented, built, flashed and on-device tested
2026-11-17.** Read Pico has the Phase 0 host (`readpico_plugins`); the shared
Phase 1 host/activities are compiled there too but have not been run on that
board.

## Package

```
/.crosspoint/plugins/<id>/manifest.json
/.crosspoint/plugins/<id>/plugin.elf
```

The scanner validates:

- safe directory/id (`[A-Za-z0-9._-]`, 1..32 chars, id == directory);
- `api == CPYOS_PLUGIN_API_VERSION` (1);
- `arch` equals the running SoC (`riscv32` on the Plus, `xtensa` on Read Pico);
- `entry == app_main`, `elf` is a simple `.elf` name, ELF ≤ 2 MiB;
- ELF32, little-endian, executable, and the matching `e_machine`.

`manifest.json` is ≤ 4 KiB. Invalid packages stay in the picker with an error
line instead of disappearing.

## ABI

`src/plugins/cpyos_plugin_api.h` is the complete contract. The loader calls
`app_main(argc, argv)`; `argv[0]` is a `const CpyosPluginApi*` and `argv[1]` is
the installed id. Fields (v1, append-only):

- `width`, `height`;
- `log`, `request_exit`, `exit_requested`;
- `now_ms`, `delay_ms`;
- `poll_event` (button down/up, tap, swipe);
- `clear`, `fill_rect`, `draw_text`, `text_width`, `present`;
- PSRAM `alloc`/`release`.

Drawing and presentation take the render lock internally. Back is the host's
exit gesture; plugins can also ask to close with `request_exit()`.

**Do not import libc.** Plugins are linked with `-nostdlib -fPIC -shared
-e app_main` and must resolve services through the table. The reference 2048
plugin has no `UND` symbols and no relocations at all (2,808 B on the Plus).

## Host behavior

- One `PluginHost` slot; the ELF task uses a 24 KiB **internal** stack (the
  loader/core touches flash/cache), while the image itself is relocated into
  PSRAM.
- Input is a bounded 16-event queue filled by `PluginActivity` from
  `MappedInputManager`; no MappedInput object crosses into the plugin.
- The activity is pushed from the plugin picker; Back closes the child and the
  input snapshot is advanced before the parent list resumes, so one Back does
  not pop both screens.
- A `.running` marker is written before relocation and removed after a clean
  `app_main` return. A crash/reboot leaves it in place; the next scan shows
  `[crashed last run]`.

## Build / install (Plus)

```bash
CPYOS_IDF_PATH=/Users/hongbin/Documents/projects/cpyos/.scratch/esp-idf-6.1 \
  bash plugins/native/2048/build.sh
```

Output `plugins/native/2048/dist/{manifest.json,plugin.elf}`. Install over the
serial console (64-byte ACK-paced `putfile`, no reboot needed if the console is
opened with `exp/plus_session.py`) into `/.crosspoint/plugins/2048/`.

## Host UI

`AppId::NativePlugins` (22) adds a `Plugins` tile (generic `Apps` glyph, existing
`STR_PLUGINS` label) under `CROSSPOINT_PLUGIN_RUNTIME`. The picker has a Retry
row that rescans the SD directory, then one row per package with version,
size/author/description, and an error or crash status.

## Verified on e4plus

- Shell build `0.3.1-plus-test1-e4p`, RAM/Flash from the `build_shell` log.
- Picker opens (`plugins` console hook), selection via `ring 1`, Confirm opens
  `Plugin`.
- Loader log: `ELF loader version 2.1.0`, `elf->entry=0x...`, plugin prints
  `2048 starting`; board renders the 4×4 grid.
- D-pad input moves/merges tiles (observed score 4 after two Down presses).
- Back returns to the picker, which stays on screen (no double-pop with the
  Phase 1 snapshot fix).
- App list can be reached from the tool drawer; the tile uses the normal
  app-visibility mask.

## Not done

- Install/update UI (copying packages is out of band).
- Per-plugin isolation beyond crash-marker reporting (a fault still reboots the
  device; no MMU sandbox).
- Read Pico on-device run and a `readpico`-arch package (build the same source
  with `arch: xtensa`).
- Plugin permission metadata, signing, and automatic catalog/download.

## Read Pico Phase 1 status (2026-11-17)

- Shared Phase 1 host/UI is compiled for `readpico_plugins` (CROSSPOINT_PLUGIN_RUNTIME
  already defined by that env). The ELF task stack is 20 KiB on Xtensa (Phase 0:
  32 KiB failed to allocate, 20 KiB held).
- Xtensa pilot: `bash plugins/native/2048/build_xtensa.sh` →
  `dist-xtensa/plugin.elf` **2,668 B**, no `UND` symbols, only
  `R_XTENSA_RELATIVE` relocations (`-ffreestanding -fno-builtin` removes the
  compiler's `memset` import).
- Firmware built with the new 6.375 MiB table:
  `readpico_plugins` Flash **5,417,243 / 6,684,672 B (81.0%)**, RAM
  **89,980 B (27.5%)**, `firmware.bin` **5,422,720 B**, app descriptor
  `crossmux` / `0.7.0-rp`.
  The shared `libesp_app_format.a` had stale metadata from another worktree;
  the local archive was restored from `artifacts/platformio-recovery/` and its
  32-byte project/version fields were corrected to `crossmux` / `0.7.0-rp`.
  CI rebuilds this library from a clean tree.
- Partition table + both app slots + pinyin/web/hyphen/i18n/emoji/core were
  written; every segment reported `Hash of data verified.` The device did not
  reboot into the app (USB-Serial/JTAG stopped answering after the write) and
  needs a physical USB replug/power cycle before the on-device run.
- Read Pico debug hooks: `CMD:PLUGINS` opens the picker, `CMD:MKDIR <path>`
  creates the package directory. The normal install path remains file transfer;
  the 2.7 KiB ELF can be pushed with `exp/putfile.py` (64-byte paced) once the
  console is back.

### Read Pico bench console note

`readpico_plugins` now defines `ENABLE_SERIAL_LOG` + `LOG_LEVEL=2` so the debug
`CMD:PUTFILE`/`CMD:MKDIR` commands exist (they are compiled out of release).
Debug firmware is **5,502,384 B**. Opening the port with pyserial (DTR/RTS
toggled) wedged the USB-Serial/JTAG unit twice; use the raw termios session from
`exp/plus_session.py` (`RawPort`) or `exp/mon_device.py` instead. A physical USB
replug restores the chip; do not chain esptool retries while it is wedged.

### Read Pico on-device result (2026-11-17, after USB replug)

The device recovered after a physical USB replug. The 6.375 MiB table and both
app slots boot (`0.7.0-rp`), the SD data partition mounts, and the plugin picker
lists `2048`. The first launch exposed a real loader bug, not a plugin bug:

- **Symptom**: `Guru Meditation (LoadStoreError)` in `GfxRenderer::drawText`,
  `EXCVADDR` pointing at plugin `.text` alias; `draw_text text=0x42...af2`.
- **Root cause**: `.text` was 0x536 bytes. The loader rounded
  `sec[text].size` up to 0x538, which made the first two bytes of `.rodata`
  (starting at vaddr 0x6f2) look like `.text`. Xtensa `l32r` literals pointing
  at `.rodata` were remapped to the instruction alias; the first data read from
  that address faulted.
- **Fix**: keep the real (unaligned) `.text` size in `sec[text].size` for
  symbol lookups; the executable allocation is still padded separately
  (`text_alloc_span`). See `native_plugins/ElfLoader/src/esp_elf.c`.
- **After fix**: 2048 renders, d-pad events move tiles (bench `ROWDOWN` maps to
  Down), `HOME` exits cleanly (`2048 exiting` / `finished`), no panic.
  `readpico_plugins` final image **5,502,880 B**, `crossmux` / `0.7.0-rp`.
- Back is the same host path as e4plus but was not pressed physically in this
  session; `HOME` verified task teardown and the parent activity transition.

## Phase 2: management surface and per-target loader parity (2026-11-17)

- **One runtime, two targets.** `work/plugin-runtime` (Read Pico) and
  `work/plugin-runtime-e4p` (Plus) were merged into one integration commit;
  the shared runtime uses the readpico fixes and each port keeps its own build
  integration.
- **Plugin manager UI** follows the CPYOS list grammar:
  - Rescan row (`重试`, refresh icon, package root, package count; the empty
    state is the existing `No plugins with device screens installed` copy).
  - One row per package with the plugin/Blocks icon, name, `version · size ·
    arch`, and `Installed` / `Error` status.
  - Touch: swipe-left or long-press opens the standard inline delete
    confirmation. Deletion removes the package directory and rescans.
  - The path stays visible in the rescan subtitle; there is no separate folder
    row (the child FileBrowser's Back/up-directory semantics did not fit a
    one-level package picker).
- **Failure surface**: plugin start/relocation failures now use the shared
  `status_card::paintMessage` card instead of a bespoke text bar.
- **Loader parity**: the unaligned `.text` size fix from Read Pico was applied
  to the Plus `components/elf_loader` as well, so both targets handle plugins
  whose `.text` is not word-aligned.
- **Back automation**: `Activity::debugBack()` + `CMD:BACK` exercise the same
  `PluginActivity::beginExit()` path as a physical Back; the Plus also has the
  shell `key back` injection. Verified on Plus: Back returns to the manager,
  manager remains on screen.
- **Not yet**: online catalog/download, signing/permissions, per-plugin
  resource limits, and a plugin SDK package.

### Post-merge build check

After the Phase 2 merge, `readpico_plugins` also builds clean:
RAM **90,004 / 327,680 B (27.5%)**, Flash **5,511,291 / 6,684,672 B
(82.4%)**, `firmware.bin` **5,516,560 B**, `crossmux` / `0.7.0-rp`.
The merged shared runtime therefore compiles for both Xtensa and RISC-V; only
the Plus was reflashed and exercised in this round.

## Phase 2 completion: catalog, integrity, limits, SDK

### Catalog and installation

- `PluginCatalog` reads the same `PEXT1` TSV from a card file
  (`/.crosspoint/plugins/catalog.txt`) or the URL in
  `CPYOS_PLUGIN_CATALOG_URL` (default GitHub Pages). File URLs
  (`file:///...`) are accepted for offline testing.
- Rows merge installed packages and catalog entries:
  - installed same version → `Installed`;
  - installed older version → `Update`;
  - catalog-only → `Not installed` and tap-to-install.
- Network work runs on a 16 KiB PSRAM worker (8 KiB internal fallback); the
  shared wait card shows progress and Cancel. It uses only the last saved
  network, like the browser's silent path. Local `file://` installs skip Wi-Fi.
- Installation downloads/copies to `plugin.part`, checks declared size and
  SHA-256, validates the ELF architecture, then replaces `plugin.elf` and writes
  a manifest containing `sha256`. A failed update leaves the old package.
- A malformed catalog or package is rejected; the card catalog is not
  overwritten by the remote cache (`catalog.cache` is separate).

### Integrity and limits

- SHA-256 is an **integrity** check, not authenticity/signing. There is no
  public-key signature or sandbox in this phase.
- `PluginHost` enforces a 2 MiB PSRAM budget and at most 32 live allocations
  per plugin; allocations are reclaimed when the plugin returns. Input is a
  bounded queue; drawing is clipped by the renderer.
- The ELF `.text` real size is used for symbol lookup on both loaders, with a
  separate padded executable allocation.

### SDK

`plugins/sdk/` contains a minimal plugin, manifests for both architectures, and
`build_xtensa.sh` / `build_e4p.sh`. Both templates build without undefined
symbols. `scripts/build_plugin_catalog.py` generates the TSV from packaged ELFs;
`plugins/catalog/README.md` documents publishing the manifest and assets.

### On-device check (Plus)

Local catalog test installed `demo` (2,808 B), updated it from 1.0.0 to 1.0.1
with replace-over-existing, and launched it (`plugin[demo]: 2048 starting` /
finished). Both targets now pass the Back smoke; the Plus also restored the 2048 board byte-identically after relaunch. The test catalog/package were removed afterwards; `2048` remains.
The remote HTTPS path was compiled but not exercised in this session because no
reachable saved Wi-Fi was available.

### Back smoke automation

`scripts/plugin_back_smoke.py --target e4plus|readpico` opens the picker,
selects the first plugin row, launches it, presses Back and asserts that the
plugin exited while `Plugins` stayed on screen. It uses raw termios for both
targets (no DTR/RTS) and is the reproducible acceptance hook for the Plus and
Read Pico.

## Phase 3: persistent plugin state and Read Pico acceptance

- **ABI append**: `CpyosPluginApi` now exposes `data_read` / `data_write`.
  Storage is always scoped to `/.crosspoint/plugin_data/<id>/`; names are simple
  `[A-Za-z0-9._-]{1,48}`, writes are capped at 64 KiB, and the host creates the
  directory on first write. There is no cross-plugin or arbitrary filesystem
  access. Old plugins ignore the appended fields via `struct_size`.
- **2048 save state**: the pilot stores board, score, RNG and game-over flag in
  `save.bin` on exit and restores it on the next launch. Verified on Read Pico:
  move → Back → reopen produced a byte-identical framebuffer (cmp 0).
- **Read Pico loader stack**: the plugin task stack moved to PSRAM (32 KiB) on
  Xtensa. With Wi-Fi and the catalog UI resident, an internal 20 KiB task
  creation failed (`cannot create the plugin task`); PSRAM restored launch.
  Internal RAM at plugin runtime stayed around 57 KiB free.
- **Catalog assets**: `site/plugins/manifest-v1.txt` plus
  `2048-riscv32.elf` / `2048-xtensa.elf` are generated by
  `scripts/build_plugin_catalog.py`; Pages will serve the default remote URL
  after the site is published.
- **Read Pico acceptance**:
  - `scripts/plugin_back_smoke.py --target readpico` → PASS.
  - Local SD catalog installed `Demo 2048` (Xtensa, 2,668 B) and listed it as
    installed alongside 2048.
  - Firmware final: RAM **90,164 / 327,680 (27.5%)**, Flash
    **5,513,163 / 6,684,672 (82.5%)**, bin **5,518,432 B**,
    `crossmux / 0.7.0-rp`.
  - Remote catalog: the device connected to the AP (`10.229.51.21`) but the Mac
    was on a different subnet (`30.128.92.189`), so the local HTTPS/HTTP
    harness was unreachable; the fetch path returned `status -1`. The local
    catalog path and installer are verified; remote publication is ready but
    still needs a reachable host/Pages deployment.

- Uninstalling a package now removes its scoped data directory as well; updates
  keep it. `site/plugins` carries the 1.1.0 persistence builds.

## Phase 4: all mini-games as plugin packages

`plugins/native/build_games.sh` builds six packages for both targets:
`2048`, `sokoban`, `gomoku`, `minesweeper`, `sudoku`, `chinese-chess`.
Every ELF is linked freestanding with no undefined imports (no libc/PLT); the
shared `game_common.h` only uses the host API and integer drawing helpers.

The packages are installed under `/.crosspoint/plugins/<id>/` by copying
`dist/{manifest.json,plugin.elf}` (RISC-V) or `dist-xtensa/...` (Xtensa).
`site/plugins` carries both architecture assets and the catalog manifest, so
the same six games are available through the plugin installer.

On the Plus, all six packages were launched and exited with Back without a
panic, and one input action per game was exercised (Sokoban move, Gomoku stone,
Minesweeper dig, Sudoku digit, Chinese Chess king move). Screenshots are in
`artifacts/phase1_plugin_runtime/e4plus/` after the next archive update.

These are playable externalized versions, not yet full parity with the built-in
menus/AI/persistence (Gomoku and Xiangqi are local two-player; Sudoku uses a
fixed puzzle; Minesweeper has no flag action; Sokoban has three levels). The
in-app games remain in non-plugin builds until parity/removal is approved.

### Phase 4 image and verification

The final Plus shell with the game-launch bench hook is **6,126,144 B**. All six
game packages were installed and launched; screenshots are under
`artifacts/phase1_plugin_runtime/e4plus/games/`. The Xtensa packages are built
and catalogued but were not re-installed/tested on the currently disconnected
Read Pico in this round.

### Full migration: built-ins removed from plugin-runtime images

Plugin-runtime builds (`readpico_plugins`; Plus with `CPYOS_SHELL_DEMO=1`) no
longer compile the six built-in games:

- `ActivityManager` includes and `goTo*` definitions, and `AppCatalog` rows and
  open callbacks, are gated on `CROSSPOINT_PLUGIN_RUNTIME`.
- The Plus app CMakeLists excludes
  `apps/{2048,sokoban,gomoku,minesweeper,sudoku,chinese-chess}` via
  `list(FILTER ... EXCLUDE)`; `readpico_plugins` uses the equivalent
  `build_src_filter` entries.
- Read Pico's `MINES` / `SOKOBAN` / `2048` bench hooks are disabled in the same
  configuration.
- Do **not** exclude `GameUi.cpp`: reading-stats and avatar still use
  `gameTouchActionRect` and the other shared helpers.

Verified after removal: e4plus image dropped from 6,126,144 B to **6,015,328 B**
(-110,816 B), all six built-in Activity symbols are absent, and
`pluginlaunch` + Back still launches/exits all six packages
(`scripts/plugin_back_smoke.py` PASS). `readpico_plugins` binary is
**5,419,456 B**, Flash **5,414,195 / 6,684,672 (81.0%)**, RAM
**90,164 / 327,680 (27.5%)**, i.e. 98,968 B less Flash than before the removal;
the six symbols are absent. The Xtensa packages still need installation on a
connected Read Pico.

Non-plugin builds keep the built-in games as a rollback path until the plugin
versions reach feature parity and the plugin runtime becomes the default.


### Tool drawer, app visibility and plugin i18n

Installed packages are first-class apps: the tool drawer appends one tile per
loadable package after the built-in entries, and the existing plugin catalog
tile remains for install/update/remove. Tapping a tile resolves the package id
and launches `PluginActivity` directly, so the plugin list is a management
surface rather than the only launcher.

The app-visibility page lists the same entries. Plugin visibility is stored by
plugin id in `/.crosspoint/plugins/.visibility` (`PXHV1`, one id per line),
not by taking bits from the 32-bit `hiddenAppsMask`. A toggle writes the file
immediately and the tool drawer reloads it on entry, so installing/removing or
reordering packages never moves another package's hidden state.

Plugins can localize their UI through the appended ABI field
`const char* (*text)(int32_t id)` (`CpyosPluginText`). The host maps those ids
to existing game translation keys, so no new i18n keys or data-partition flash
are required. Older plugins ignore the field through `struct_size`; the
`cpyos_plugin_text()` helper returns an empty string when the host predates it.
The six game packages now use it for titles, score, wins/clears, restart and
input hints. Board glyphs, piece characters and digits remain language-neutral.
The plugin manager also uses the localized game titles for installed/catalog rows.
