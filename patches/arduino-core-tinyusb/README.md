# TinyUSB Kconfig symbols the precompiled Arduino core does not define

> ## ⚠ SUPERSEDED — do not use this approach. Kept for the analysis and the fragments.
>
> ★ **Status of the env it describes:** `[env:readpico_usbmsc]` still exists, but as
> an **opt-in** build. USB MSC is merged and off by default: the flags live in the
> `[readpico_usb_msc]` block in `platformio.ini`, which only that env pulls in, so a
> normal `pio run -e readpico` produces a lean image with no U-disk entry. What the
> `-D` route below replaced is still true and still lives in that block's comments.
>
> Injecting into `framework-arduinoespressif32/Kconfig.projbuild` **cannot survive a build**.
> Changing `custom_sdkconfig` makes pioarduino print `*** Reinstall Arduino framework ***`
> and re-unpack the core tarball, which restores that file from the archive (mtime reverts to
> the package's original) and deletes anything else you put in the package directory —
> including this patch's own `.crossmux-bak`. Measured, not inferred: after one
> `pio run -e readpico_usbmsc` the injection was gone and the generated
> `sdkconfig.readpico_usbmsc` had 0 TINYUSB lines, so the three `custom_sdkconfig` settings
> had been dropped as unknown symbols and the build failed with
> `UsbMassStorage.cpp:21: error: 'USBMSC' does not name a type`.
>
> **What works instead:** define the macros as preprocessor flags in the env's
> `build_flags` (`-DCONFIG_TINYUSB_ENABLED=1` …). The component's own `tusb_config.h:39-76`
> guards every class with `#ifndef CONFIG_TINYUSB_<x>_ENABLED / #define … 0`, i.e. it is
> written to receive these from the preprocessor. Flags also survive the reinstall, are
> versioned in `platformio.ini`, and are scoped to one env — which removes the leak hazard
> described at the end of this file. See the comment block on `[env:readpico_usbmsc]`.
>
> **The constraint that flags alone do not satisfy** — see "Matching the prebuilt archive"
> below. Getting this wrong moves the failure from compile time to link time.

`[env:readpico_usbmsc]` hands the TF card to a PC as a USB mass-storage device. It asks
for that with three `custom_sdkconfig` lines:

```ini
CONFIG_TINYUSB_ENABLED=y
CONFIG_TINYUSB_MSC_ENABLED=y
CONFIG_TINYUSB_MSC_BUFSIZE=4096
```

On a stock pioarduino install those three lines do nothing. **The symbols are not defined
anywhere**, so IDF's configuration generator treats them as unknown and drops them, and the
Arduino core compiles `esp32-hal-tinyusb.c` / `USB.cpp` / `USBMSC.cpp` with TinyUSB switched
off. The result is a build that succeeds and links no MSC support at all — the failure is
silent, which is what makes this worth writing down.

Why they are missing — the mechanism, measured rather than inferred. The TinyUSB *code* is
not the problem: `framework-arduinoespressif32-libs/esp32s3/lib/libarduino_tinyusb.a`
already carries 28 `tud_msc`/`mscd_*` symbols, and the vendor's
`framework-arduinoespressif32-libs/esp32s3/sdkconfig` has all 33 `CONFIG_TINYUSB_*` lines
set. What is missing is the **macro**, in the four core files that are compiled from source
rather than shipped prebuilt — `cores/esp32/USB.cpp`, `USB.h`, `esp32-hal-tinyusb.c` and
`.h`, each gated on `#if CONFIG_TINYUSB_ENABLED`. That macro comes from a generated
`sdkconfig.h`, which comes from Kconfig, and:

- the `arduino_tinyusb` component ships in `…/include/arduino_tinyusb/` as **headers plus a
  prebuilt archive only** — its `Kconfig` was stripped from the package;
- so nothing under `~/.platformio/packages` defines the symbols. Verify:
  `find ~/.platformio/packages -name 'Kconfig*' | xargs grep -l TINYUSB_MSC_ENABLED`
  prints nothing;
- and this project's own custom-rebuild config proves it end to end:
  `…/esp32s3/sdkconfig.crosspoint` contains **0** TINYUSB lines, against 33 in the vendor
  `sdkconfig` sitting next to it.

With the symbols undefined, `sdkconfig.h` omits the macro, those four files compile to
nothing, and the archive's MSC code is never referenced.

## Why not take the prebuilt-core route instead

`platformio.ini` warns, above `[firmware_tuned]`, that USB-OTG profiles "need PioArduino's
complete prebuilt TinyUSB component graph, which the custom core rebuild omits", and
`[x4pro_hardware]` says it "deliberately extends base rather than firmware_tuned" for that
reason. On those boards that works: they have no `custom_sdkconfig` to lose.

**Read Pico cannot do this.** Its `custom_sdkconfig` (see `[readpico_hardware]`) carries
board-critical overrides that only a rebuilt core can apply:

- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` / `CONFIG_ESP_CONSOLE_UART_DEFAULT=n` — this board
  routes touch INT# to GPIO43 and TF-card D0 to GPIO44, which *are* UART0. Leaving the IDF
  console on UART0 drives the SD card's data line as a UART TX. The card is precisely what
  MSC exposes, so losing this defeats the feature.
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` — epdiy's LCD path needs ~45 KB of
  *contiguous* internal RAM or the panel goes dead with no message.
- `CONFIG_ESP32S3_DATA_CACHE_64KB=y` — the framebuffer streams out of PSRAM row by row.

A prebuilt core cannot take any `custom_sdkconfig`, so "extends base" would trade a working
console and a working panel for a component we can supply ourselves. Injecting the symbol
definitions keeps both: the custom rebuild still applies the board overrides, and the four
gated core files now compile against the MSC code that was in the archive all along.

That conflict — readpico needs the rebuild, MSC needs the component the rebuild omits — is
the whole reason this patch exists. Do not "simplify" it by copying the x4pro shape.

## What `apply.sh` does

Injects `tinyusb.kconfig` — 33 symbol definitions — into
`framework-arduinoespressif32/Kconfig.projbuild`, just before the closing `endmenu` of the
file's single top-level menu. Wrapped in marker comments, so:

```sh
./apply.sh            # inject; idempotent, refuses to touch a file it does not recognise
./apply.sh --status   # which state is the core file in
./apply.sh --revert   # remove the block again
```

`--revert` is exact: afterwards the core file is byte-identical to
`Kconfig.projbuild.pristine`, which is the stock file this patch was derived from
(13,270 B, `md5 846916aac41a3fbcd6e9e614c82ee8f9`). Re-running `apply.sh` after a core
upgrade is safe — it only refuses if the file mentions `TINYUSB` without our markers,
which would mean the core grew its own definitions and this patch should be retired.

The first injection leaves a one-time `Kconfig.projbuild.crossmux-bak` next to the target.

**After applying or reverting, the generated configs are stale.** Delete them or the next
build reuses the old resolution:

```sh
rm -f sdkconfig.readpico_usbmsc && rm -rf .pio/build/readpico_usbmsc
```

Because `custom_sdkconfig` changed, the next build also **recompiles the Arduino core from
source** (≈7 min, and it fills `.cache` — see `AGENTS.md` §4.5 and `exp/disk_hygiene.sh`).

## `tinyusb.kconfig` is a flattening, not a copy

`tinyusb.upstream.kconfig` is the component's original nested menu, kept so the derivation
can be re-checked. `tinyusb.kconfig` is the same 33 symbols with the gating removed. Three
consequences, all deliberate:

1. **`depends on` clauses are gone.** Upstream wraps the menu in
   `depends on ENABLE_ARDUINO_DEPENDS && SOC_USB_OTG_SUPPORTED` and gates each symbol on its
   class. `SOC_USB_OTG_SUPPORTED` is not visible in this Kconfig's scope, so the whole menu
   stayed invisible; that is the bug being worked around.
2. **Conditional defaults became unconditional.** Upstream writes
   `default 64 if IDF_TARGET_ESP32S2 || IDF_TARGET_ESP32S3` / `default 512 if IDF_TARGET_ESP32P4`.
   Kconfig takes the *first* matching default, so both being unconditional means **64 always
   wins**. Correct for the S3; wrong for an ESP32-P4, which this project does not target.
   Revisit before porting.
3. **`TINYUSB_ENABLED` lost its two `select`s.**
   `FREERTOS_SUPPORT_STATIC_ALLOCATION` is already `y` in the base configuration, so that one
   costs nothing. `FREERTOS_USE_AUTHENTIC_INCLUDE_PATHS` stays off and the build works anyway,
   because the env supplies the two `-I` flags into `arduino_tinyusb/` explicitly.

Re-derive the flat file from the nested one if the component's Kconfig ever changes; do not
hand-edit `tinyusb.kconfig` alone, or the two will disagree silently.

## Scope: this touches a shared package, not the repository

The injection lives in `~/.platformio/packages/`, outside git, and the symbols become
visible to **every** env that uses this core — not just `readpico_usbmsc`. Every class in
the fragment defaults to `y`, so an env that does not ask for TinyUSB could still resolve
these symbols as enabled.

That is a real hazard, and it is why the rule below is not optional:

> **After applying this patch, rebuild plain `readpico` and confirm it gained no USB code.**
> Check the ELF for `tud_*` / `mscd_*` / `UsbMassStorage` symbols (expect **0**), *not* by
> byte-comparing against an older `firmware.bin` — see "Regression" below for why `cmp`
> against `device/readpico_known_good_20261016.bin` proves nothing.

If `readpico` ever does pick TinyUSB up, the fix is to change the class-enable bools in
`tinyusb.kconfig` to `default n` and let the env turn on exactly the classes it needs.
★ That idea does not survive contact with the linker: the archive was built with every
class on, so a reduced set leaves its callbacks undefined — see "Matching the prebuilt
archive" below. This whole section is moot anyway, since the `-D` route replaced it.

## Matching the prebuilt archive (the part that is easy to get wrong)

Enabling only `TINYUSB_ENABLED` + `TINYUSB_MSC_ENABLED` **does not link**. Measured:

```
libarduino_tinyusb.a(hid_device.c.obj):  undefined reference to `tud_hid_descriptor_report_cb'
libarduino_tinyusb.a(hid_device.c.obj):  undefined reference to `tud_hid_get_report_cb'
libarduino_tinyusb.a(hid_device.c.obj):  undefined reference to `tud_hid_set_report_cb'
libarduino_tinyusb.a(dfu_rt_device.c.obj): undefined reference to `tud_dfu_runtime_reboot_to_dfu_cb'
libarduino_tinyusb.a(dfu_device.c.obj):  undefined reference to `tud_dfu_get_timeout_cb'
libarduino_tinyusb.a(dfu_device.c.obj):  undefined reference to `tud_dfu_download_cb'
libarduino_tinyusb.a(dfu_device.c.obj):  undefined reference to `tud_dfu_manifest_cb'
libarduino_tinyusb.a(ncm_device.c.obj):  undefined reference to `tud_network_recv_cb'
```

Why: `libarduino_tinyusb.a` was compiled by Espressif against the vendor configuration in
`framework-arduinoespressif32-libs/esp32s3/sdkconfig`, which turns **every** class on
(CDC, MSC, HID, MIDI, AUDIO, VIDEO, DFU_RT, DFU, VENDOR, NCM — 33 `CONFIG_TINYUSB_*` lines).
Those class drivers call back into the application, and the callbacks live in the core files
that are compiled **from source** (`USBHID.cpp`, the DFU/NCM units, …) and are gated on the
matching `CONFIG_TINYUSB_<class>_ENABLED`. Turn a class off on our side and its half of the
archive still gets linked but its callbacks are now missing.

`tusb_config.h`'s `#ifndef … 0` fallbacks make the mismatch **silent until link time**, so
"only enable what I use" looks reasonable and is not.

⇒ **The set you define must match the set the archive was built with.** That is why
`tinyusb.kconfig` here has `default y` on all 33 symbols: it was not sloppiness, it was
mirroring `…/esp32s3/sdkconfig`. Any `-D` replacement has to carry the same values
(`CONFIG_TINYUSB_MSC_BUFSIZE=4096` etc.), not just the MSC ones.

Two details worth keeping:

- `CONFIG_TINYUSB_MSC_BUFSIZE` has **no** `#ifndef` fallback — `tusb_config.h:154` expands it
  straight into `CFG_TUD_MSC_BUFSIZE` — so it must always be given explicitly.
- `CONFIG_TINYUSB_DESC_PRODUCT_STRING` / `_MANUFACTURER_STRING` / `_SERIAL_STRING` /
  `_BCDDEVICE` are referenced **only** inside `TINYUSB_CONFIG_DEFAULT()`
  (`esp32-hal-tinyusb.h:68-74`), and that macro is never expanded anywhere in the core or in
  `freeink-sdk/libs/hardware/UsbMassStorage`. They are not in the vendor's 33 either.
  So they do not need defining — but if a future core version expands that macro, they will.

## Files

| File | Role |
| --- | --- |
| `apply.sh` | inject / revert / status |
| `tinyusb.kconfig` | the 33 flattened symbols that get injected |
| `tinyusb.upstream.kconfig` | the component's original nested menu, for re-deriving the above |
| `Kconfig.projbuild.pristine` | the stock core file, as the revert target and diff reference |

## Reference measurement

### Current known-good — `-D` route, 2026-10-16 12:42 (`readpico_usbmsc`, build 17)

The first build of the pure-preprocessor route that **links**. The three MSC flags alone
fail at link time with the eight callbacks under "Matching the prebuilt archive" above, so
the env also carries `CONFIG_TINYUSB_{HID,DFU,DFU_RT,NCM}_ENABLED` plus the HID/DFU buffer
sizes. Verified in the ELF: `prepareUsbOtgHandoff` + `mscd_init` present, strings
`USB Drive MSC initialization failed` and `TinyUSB MSC` present.

```
RAM:   36.7% (used 120128 bytes from 327680 bytes)
Flash: 94.4% (used 6186135 bytes from 6553600 bytes)   — firmware.bin 6,191,392 B
```

Image kept at **`device/readpico_usbmsc_20261016.bin`**
(sha256 `80eeb8cc32be18d5dcff94d51718509a5c3b20d24ae799d35e4b135f7e4f0837`), because
`.pio/build/<env>` is disposable in this repo — building one env can delete another env's
directory. Build log kept at `reference-build17.log` in this directory.

### Regression: `readpico` is unaffected (same session)

Rebuilt `readpico` from HEAD `d2f93187`+dirty: **0** `tud_*`/`mscd_*`/`UsbMassStorage`
symbols in the ELF. The only "USB Drive" strings are the i18n table values every build
carries, plus one pre-existing capability-guard message — no contamination. Our source
changes contribute 0 bytes: the no-op `prepareUsbOtgHandoff()` stub is dropped by
`--gc-sections` and `UsbDriveActivity.cpp` compiles out for this env. Readpico: RAM
87,152 B / Flash used 6,125,703 B (firmware.bin 6,130,960 B).

★ **Do not byte-compare against `device/readpico_known_good_20261016.bin`** (6,130,976 B).
That image was built with HEAD `ba04b7df` and four UI commits landed between the two
builds, so the images legitimately differ everywhere the UI work touched. The
`esp_app_desc` at offset 0x20 records the HEAD hash and build time — use it to attribute a
build, not `cmp`. (A `-16 B` size delta and ≈2.3 M differing bytes looked alarming until
the desc showed `ba04b7df-dirty` vs `d2f93187-dirty`.)

### Historical — build 15, 2026-10-16 05:43 (injection route; log and image are gone)

Longer-route context, kept because the cost estimate is still the one that matters: the
shipping `readpico` at 05:5x was RAM 87,160 B / Flash 93.2%, so the MSC variant costs
roughly **+32 KB internal RAM** and **+0.9% Flash**. Internal RAM is the scarce resource on
this board (§7 of `AGENTS.md`), which is the main reason this stays a separate
verification env rather than being folded into `readpico`. Its details (`/tmp/usbmsc_build15.log`,
`.pio/build/readpico_usbmsc`) no longer exist — `/tmp` does not survive, and later builds
wiped the build directory; the 12:42 row above is the one to cite.
