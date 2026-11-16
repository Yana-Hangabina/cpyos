# CPYOS on Metalio E-INK4 Plus — OTA-preserving plan

Status: analysis + first measurements (2026-11-09). The bring-up probe is the
only CPYOS image that has run on the unit so far; nothing in this document has
been installed yet.

Goal (user decision): bring the **full CPYOS experience from Read Pico** to the
Plus, **keep its built-in OTA working**, then use the better hardware (16 MB
PSRAM, S31 at 300 MHz, 4.7" 1216x684 16-level panel) to add what the Plus has
and Read Pico does not.

## 1. What the references actually do

### 1.1 CPYOS itself already has OTA

Read Pico is **not** a single-slot/no-OTA layout. `partitions_readpico.csv`
(16 MiB) is:

```
nvs      0x009000 0x005000
otadata  0x00e000 0x002000
app0     0x010000 0x730000   (7.19 MiB, ota_0)
app1     0x740000 0x730000   (7.19 MiB, ota_1)
spiffs   0xe70000 0x180000   (1.5 MiB)
coredump 0xff0000 0x010000
```

The implementation lives in shared code and is **already compiled by the Plus
port's `cpyos_app` component**:

- `src/network/OtaUpdater.{h,cpp}` — release manifest over several mirrors,
  version/device check (`WRONG_DEVICE_ERROR`), fastest-URL probe, `esp_ota_begin`
  / `esp_ota_write` / `esp_ota_end` / `esp_ota_set_boot_partition`, and a data-pack
  manifest that writes `spiffs` with `esp_partition_erase_range`/`write`.
- `src/activities/settings/OtaUpdateActivity.{h,cpp}` — the UI, channel choice
  (stable/nightly), progress, and the 240 MHz lock during download.
- `CPYOS_OTA_MANIFEST_URL` in `platformio.ini`, `scripts/build_cpyos_release.py`
  (dual-slot manifests) and `docs/cpyos/release.md`.

The partition change the user made was only to *grow* the CrossMux slots; OTA
was kept. The Plus port must therefore use the same dual-slot structure, not
the legacy single-slot `partitions.csv`.

### 1.2 Vendor and kiikoread

| | Partition table | OTA |
|---|---|---|
| Vendor 2.0.74 | ota_0@0x80000 5M, ota_1@0x580000 5M, model/resources/font_data | cloudzao burner + in-app |
| kiikoread Plus | ota_0@0x10000 5M, ota_1@0x510000 5M, **fat storage 5.875M**, coredump | `src/main/ota_online.c` + `esp_ota_*`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, `online_ota: true` |
| CPYOS Read Pico | dual 7.19M + 1.5M spiffs | OtaUpdater + manifest + packs |
| CPYOS Plus today | single 14.375M app (legacy port table) | code linked, no matching table |

## 2. Partition table for the Plus

Both boards are 16 MiB, so the Read Pico table **fits unchanged** and keeps the
whole OTA/pack pipeline (offsets included) identical:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x730000
app1,     app,  ota_1,   0x740000, 0x730000
spiffs,   data, spiffs,  0xe70000, 0x180000
coredump, data, coredump,0xff0000, 0x10000
```

That gives **7,536,640 B per OTA slot** and the same spiffs pack layout
(pinyin 0, web 0x56000, hyphen 0x90000, i18n 0x100000, emoji 0x160000 — the
reserved factory 12 bytes at 0x80000 stay untouched).

## 3. Size budget (measured)

`build_shell` (real ActivityManager UI + book engine + probe harness):

```
cpyos_metalio_plus.bin   9,279,088 B
```

A 7.19 MiB slot needs **−1.74 MB**. Planned savings, no product feature lost:

1. **Drop 4 of the 5 built-in CJK bitmap faces** (~1.05 MB). The vector system
   pack already provides every UI/reader size at 4-bit; keep `notosans_cjk_12`
   as the emergency/offline face exactly like Read Pico (`offlineReaderFont`).
   (`shell_demo` currently registers 8/10/12/14/16 = 2.01 MB.)
2. **Move the web pages to `web_pack` in spiffs** (`kFilesPageZh/En` ≈ 0.46 MB
   still embedded in `libmain`).
3. **`CONFIG_COMPILER_OPTIMIZATION_SIZE`** for the non-hot code (~0.5–0.7 MB);
   keep `OPTIMIZATION_PERF` only where measured (rendering/decoder).
4. Remove the probe/demo harness from the product image.

Expected ≈ 6.8–7.1 MB, i.e. fits with a few hundred KB margin (Read Pico's own
release image is 7,277,968 B in this slot).

Note: the 2.55 MB vector font stays embedded as the 1.64 MB deflate carrier in
app0 (as on Read Pico), so a firmware OTA keeps text working without touching
spiffs. Data packs are updated separately by the existing pack manifest.

## 4. Work packages

### P0 — OTA-capable table (destructive, one-time)

- Ship `partitions_plus_ota.csv` = the Read Pico table above.
- Extend `flash_metalio_plus.py` with `--layout port-ota`:
  - full 16 MiB backup required (keep only the factory one; add a
    `--reuse-backup` flag so a verified factory backup is not copied per flash),
  - writes table + app0 + app1 + the five data packs, never the vendor
    bootloader,
  - refuses on secure boot/encryption, verifies the table is actually the new
    one, and reads the app header back.
- Acceptance: boot banner shows the Plus build; `otadata` selects app0; an
  `esp_ota_*` write to app1 boots and can switch back.

### P1 — Full CPYOS UI boots

- Product entry point: `ActivityManager` + `InxRecentActivity` home; no probe
  demo code.
- Fonts: vector system pack + `typography::apply()` (already proven on glass in
  the probe) and the emoji face from spiffs; only `notosans_cjk_12` built in.
- `CrossPointSettings` persistence on SD `/.crosspoint/settings.json`.
- Acceptance: Home / Library / Settings / Reader / cover thumbnails, 16-level
  AA, touch + cover keys, no `[ERR]`.

### P2 — OTA wiring for the Plus

- Reuse `OtaUpdater`/`OtaUpdateActivity` unchanged where possible.
- New release assets for the device (`plus-ota.json`, `plus-packs.txt`) and
  `CPYOS_OTA_MANIFEST_URL` for the Plus env; device check string must match.
- Pack manifest offsets come from the same table, so the existing generator
  works. Firmware-only releases keep spiffs untouched; pack changes require a
  full-install note (same rule as Read Pico).
- Acceptance: manifest check, app-only OTA to the inactive slot, reboot into
  the new version, and a pack update that leaves the other blobs intact.

### P3 — Storage, time, power

- SD settings/library/progress (CPYOS model).
- PCF8563 behind `halClock` (read works; write/NTP unverified).
- BQ27220 behind the CPYOS battery API; charge state in the status bar.
- Sleep: no CW32 PMU on this board. Decide between `esp_deep_sleep` and TPS
  rail-off (vendor behaviour), then port `StandbyActivity`/sleep screens.
- USB MSC (TinyUSB already linked) and the USJ/UTMI route on GPIO36.

### P4 — Plus-specific capabilities

- Front light: cool/warm PWM on GPIO6/7 behind `HalFrontlight` (stub today);
  keep the vendor clamp until measured on glass.
- IMU: SC7A20H behind `HalTiltSensor` for tilt/shake page turns (the driver is
  already compiled; only the board mapping is missing).
- BLE HID page-turner (NimBLE central, bonds in NVS), mirroring kiikoread.
- Performance: 300 MHz CPU / 200 MHz PSRAM, 4 page-cache slots, 1 MiB TTF
  cache, ZIP prefetch — kiikoread's tuned values, cheap to mirror.
- 4G / AI voice / WeChat: separate module firmware (NT26K / BH1098G); out of
  scope until documented, and never faked in the ESP firmware.

## 5. Risks / limits

- **Repartition is destructive**: it overwrites `model`, `resources` and
  `font_data`. The factory 16 MiB backup (`device/plus_full_backup_20261109.bin`,
  verified vendor 2.0.66 / `Metalio-E-Ink4-Plus`) is the only way back. Keep
  only that one; do not accumulate per-flash copies.
- Stock bootloader (IDF 5.5.4) already boots this app. OTA uses app-side
  `esp_ota_*`; no bootloader change is needed. Rollback
  (`BOOTLOADER_APP_ROLLBACK_ENABLE`) is **not** enabled: it requires
  `esp_ota_mark_app_valid_cancel_rollback()` on first boot, otherwise every
  reboot rolls back.
- **Battery safety**: vendor 3.1 V BATFET cutoff and software power-off are not
  ported. Keep USB power until P3 lands.
- **VCOM** stays the vendor 1200 mV until this unit is measured.
- **AI key** is not an ESP32 input (belongs to the voice module).
