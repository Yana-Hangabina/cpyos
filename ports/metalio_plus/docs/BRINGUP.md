Status at a glance (2026-11-11, latest first):

| # | Item | State |
|---|---|---|
| 1 | SD card does not answer (INVALID_CRC at 1-bit/1 MHz, with and without the PSRAM bounce) | fixed: internal 4 KiB aligned DMA + no host deinit between profiles; mounts at 40 MHz attempt 1 and survives write load |
| 2 | Console "wedged" (0 bytes on open, esptool cannot connect) | **root causes found**: 4 KiB console task stack panicked on `stat`; serial opens chained back-to-back; boot could stop between I2C init and DRAM (now self-restarting via a 60 s boot watchdog) |
| 3 | SD mount ladder poisoned after one failure (INVALID_STATE for every later profile) | fixed |
| 4 | Missing card now shows the shared retryable storage-error page instead of an empty library | fixed + verified |
| 5 | Display settings category paints the first two glyphs of each label 26 px lower | open, instrumented |
| 6 | Binary uploads corrupted (`putfile`) | fixed: console RX line endings forced to LF; probe + JPEG + CBZ hash identically on host and card |
| 7 | Forced JPEG failures (`JPEG_DECODE_ERROR`) | explained by #6; `imgtest` passes all three configs on 3 JPEGs, card stays mounted |
| 8 | CBZ comic reading | verified on hardware: 3-page CBZ opens and renders |
| 9 | Bench images start with sleep timeout "never" | verified: `info` reports `sleepTimeoutMinutes=31` (one-time migration, setting stays usable) |
| 10 | Protected (JJWXC) EPUB opens | fixed: device secret warmed on the main task before the PSRAM-stack render task exists; `/JJWXC/...epub` decrypts and renders |
| 11 | Frontlight | enabled: port `compat/HalFrontlight` drives GPIO6/7 (10-bit, 20 kHz) with the **vendor 10 % cap** (`duty = 1023*b*10/10000`); `light 100 50` -> 102/102, `80 40` -> 81/64, `0` -> 0/0. The earlier "half-scale" mapping (511 at 100 %) was 5× the vendor current and has been removed (2026-11-13) |
| 12 | PNG pages in CBZ | verified with a 3-page PNG comic; the earlier failure is specific to the older `plus-test.cbz` entry layout |

# Metalio E-INK4 Plus — bring-up log and first-power-on checklist

**Everything here is UNTESTED. This checklist exists so the first session on a real
unit produces evidence instead of guesses.** The consolidated pass/fail matrix
(including the software gates that must be green before flashing) is
[`ACCEPTANCE.md`](ACCEPTANCE.md).

## 0. Before power

1. Back up the **entire 16 MiB flash** of the unit first and keep it off-machine.
   There is no factory image in this repository.
2. **Confirm/record VCOM.** The port writes the vendor default **1200 mV**
   (`config.h`). The reference read-pico unit measured **3000 mV** for its
   different panel batch; do **not** copy that value without measuring this
   unit — the vendor firmware and this port agree on 1200 mV. A wrong VCOM can
   permanently damage the panel. Keep the first sessions short; measure before
   sustained reading if the image is very light/ghosty. The boot log prints the
   value actually written.
3. Have the USB-Serial/JTAG port open at 115200 and a way to power-cycle the board.
4. **Entering download mode:** this board has no separate reset button. If
   `esptool` reports `No serial data received`, hold **BOOT (GPIO61)** while
   power-cycling / tapping POWER, release BOOT after the USB port re-enumerates,
   then retry. The port may change (`/dev/cu.usbmodem*`); close any serial
   monitor first. `source $IDF_PATH/export.sh` so `python3 -m esptool` resolves
   to the IDF esptool with `esp32s31` support.
5. **Keep USB power connected for the first sessions.** The port keeps the
   charger read-only and does **not** port the vendor's 3.1 V BATFET cutoff or
   software power-off. Do not leave it running on battery until those are
   ported; see `docs/engineering/metalio-eink4-plus-gaps.md` §2a.

## 1. Flash (stock layout first — do not use `idf.py flash`)

The stock Metalio has **two 5 MiB app slots** (`ota_0@0x80000`,
`ota_1@0x580000`), not the CPYOS single-slot table. `idf.py flash` would also
write this project's bootloader/partition table and destroy the vendor layout.

For the first hardware session, write the **bring-up** image (3.3 MB, fits the
stock slots) to both stock slots after a full backup:

```bash
source $IDF_PATH/export.sh
python3 scripts/flash_metalio_plus.py --layout vendor --slot both \
    --firmware ../../../../artifacts/cpyos_metalio_plus_bringup_20261109_untested.bin \
    --backup /tmp/plus-full-16m.bin
```

The planner preflights the chip (`ESP32-S31`), the 16 MiB flash and the real
partition table at 0x8000, then writes only the app slot(s). It never writes the
bootloader, partition table, otadata, nvs, coredump, the vendor
`model`/`resources`/`font_data` partitions or the reserved spiffs bytes. The full
shell is 9.28 MB and **cannot** be flashed in the stock layout; use the reader or
bring-up image for the first pass.

A CPYOS single-slot layout (`partitions.csv`, 14.375 MiB app + 1.5 MiB spiffs) is
available only as a destructive `--layout port --full-install` after a full
backup. The planner refuses it when `otadata` is initialised, because that table
has no `ota_1` and a stock `otadata` could select the middle of the new image.

The planner also runs `espefuse summary` and refuses a unit with secure boot or
flash encryption enabled (the unsigned image would be rejected, and a vendor
layout install would already have overwritten both slots). If `espefuse` cannot
read the unit, inspect it manually; only after confirming
`SECURE_BOOT_EN=False` and `FLASH_CRYPT_CNT=0` may you pass
`--skip-efuse-check`. A full install writes the table last and reads the app
header back; an interruption is not resumable from the device (use BOOT/download
mode and the 16 MiB backup).

★ **Cross-version boot is unverified.** The stock bootloader is IDF 5.5.4; this
app is built with IDF 6.1 preview. The image format is expected to be compatible,
but if the first boot does not reach the bring-up banner, power off and restore
the 16 MiB backup instead of flashing repeatedly. A full install can optionally
write this project's bootloader later, but that is a separate, riskier step.

## 1a. First real-unit results (2026-11-09)

The bring-up image was written to both stock slots and booted from `ota_0`
(`otadata seq=1`). Software-side evidence:

```
ESP32-S31 rev v0.0, 16 MB flash, 16 MB PSRAM @200 MHz, CPU 320 MHz
boot: released EPD/SD/I2C/FL gpio holds
I2C1: 0x19, 0x20, 0x38, 0x51, 0x55, 0x68, 0x6B all ACK
TCA/PCA9555 ACK at 0x20; ioexp=ok
TPS PowerOn ok vcom=1200 mV; ready native=1216x684
display bring-up pattern pushed (1216x684); 48 frames / 568 ms
FT6336U ok chip=0x64 (FT6336) fw=0x03
SD mounted 4-bit at 40000 kHz
BQ27220 4368 mV 100%; PCF8563 seeded the clock
```

**Two important hardware lessons:**

1. **P14 must stay HIGH.** Cutting P14 (4G/BT-audio/PA) after init made the
   *next* reset report `TPS probe failed: ESP_ERR_NOT_FOUND` and no TCA/TPS
   rails: the TCA output latch survives a CPU reset and P14 also feeds the
   TCA/TPS path on this unit. The call was reverted; P14 behaves like the
   vendor Wi-Fi mode. Do not add a 4G power-down until the rail tree is measured.
2. **The UI is portrait-logical, the panel is native landscape — and only one
   layer may rotate.** Vendor 2.0.66 does `(xL,yL) -> (yL, 683-xL)` in
   `epd_logical_to_physical`; kiikoread gets the same result by setting
   `EPD_ROT_INVERTED_PORTRAIT`. CPYOS's `GfxRenderer::rotateCoordinates(Portrait)`
   already applies exactly that formula, so `HalDisplay` must copy the physical
   1216x684 buffer 1:1. Applying it again in `HalDisplay` rotated twice and
   produced the first unit's "top half black + frame" image. Verified on glass
   with a held TL-filled / BR-filled orientation frame, after which the
   orientation test was removed.
3. **`epd_draw_pixel` wants the 4-bit grey in BOTH nibbles.** It writes
   `color & 0xF0` for odd x and `color >> 4` for even x, so passing `0x0F` or
   `0x00` stores 0x00 for both parities: a correctly drawn white page came out
   completely black. White must be `0xFF`, black `0x00`, and grey `v | (v<<4)`.
   This was the actual cause of the "full black screen" after the rotation was
   fixed.
4. **Text clarity needs the vector face.** The built-in `notosans_cjk_12` is
   2-bit (four grey levels), so the 16-level commit alone cannot smooth it.
   The port now compiles `src/SystemFontPack.cpp` + `src/Typography.cpp` with
   `CROSSPOINT_SYSTEM_FONT_PACK` / `_FONT_PACK_EMBEDDED` and embeds the same
   `assets/fonts/cpyos_han_sans.def` carrier Read Pico uses (2,553,580 bytes,
   4-bit coverage, inflated once to SD and streamed). Verified on glass:
   text is anti-aliased and the added 16-step ramp (y=800..900) shows a smooth
   grey progression. The probe image is now 5,216,928 B, i.e. only ~26 KB of
   the stock 5 MiB slot left.
5. **The AI voice key is not an ESP32 key.** The vendor key table has only
   BOOT / volume+ / volume- / power; the AI key belongs to the external
   voice/BT-audio module, so the firmware sees no event. Touch, both cover
   page keys, the menu key, all three volume keys and the power key have been
   observed producing events.
6. **The 2.0.74 streaming render/LCD refactor** (prefill tasks, NULL dirty
   masks) was reverted to the stock 2.0.66 render stack, which is the version
   that actually paints on this unit.
7. **USB-Serial/JTAG disappears after the I2C init** because I2C takes GPIO0/1.
   The vendor also uses UART0 on GPIO58/59 as its console; USB is for flashing
   (ROM) and HS MSC, not an interactive runtime console. The bring-up image has
   no command console; it logs a 2 s heartbeat after USB re-enumerates. If you
   need commands, use a UART0 adapter (58/59) or the shell image over UART0 —
   `bringup_serial.py` does **not** work against the bring-up image over USB.
   The i18n pack error is expected on the stock layout (no CPYOS spiffs); the
   bring-up UI falls back to English and does not crash.

The bring-up screen uses the CPYOS renderer with the vector system face
(`UI_12` / typography-bound status bar), so both the CJK body text and the
themed status bar render. Verified on glass: portrait orientation, no mirroring,
flush 3 px border, anti-aliased text and a smooth 16-step grey ramp; touch
coordinates track the finger, both cover page keys, the menu key, all volume
keys and the power key produce events. The 1216x684 panel scan and TPS/PGOOD
sequence above remain the low-level evidence.

The 2 s heartbeat prints `tps68=0` after boot **by design**: the TPS is returned
to SLEEP by `epd_poweroff()` and only ACKs I2C while awake. The boot-time scan
did find `0x68 ACK`, and `epd=1` is the proof that TPS power-on succeeded. The
heartbeat also shows `events=0, touch=-1,-1` until someone touches the panel or
presses a cover key; each event increments `events` and logs `UI-PROBE event ...`.

## 2. Boot log (BSP build)

Expect, in order:

```
CPYOS-PLUS: Metalio E-INK4 Plus bring-up build - UNTESTED ON HARDWARE
CPYOS-PLUS: chip=... cores=... flash=... psram=...
MetalioEpd: EPDiy init E0470A14-AF 34Pin VCOM=1200 mV
MetalioEpd: TPS PowerOn ok ...
CPYOS-PLUS: display bring-up pattern pushed (1216x684)
CPYOS-PLUS: FT6336U touch ready
CPYOS-SD: mounted 4-bit SD at /sdcard
PLUS-PWR: BQ27220 <mV> mV <mA> mA <soc>%
```

Triage:

| Symptom | Where to look |
|---|---|
| No panel image, no obvious crash | TPS65185/TCA9555 sequencing, PGOOD; VCOM value |
| Garbled/ghosted image | Waveform/board revision mismatch; do not guess — capture a photo |
| `FT6336U did not answer` | Touch reset on TCA P1.2, address 0x38, GPIO5 INT |
| `SD mount failed for every profile` | Card seating, 3.3 V rail, SDMMC pins 24–29; the log lists each tried width/speed |
| `BQ27220 not answering` | I2C1 (SDA0/SCL1), gauge at 0x55 |

## 3. Shell build (`CPYOS_SHELL_DEMO=1`)

Boot starts with `plus boot: reset=<n> wake=<n>` (cold/watchdog/panic/brownout vs wake-up; compare with `slpinfo`), then logs `CPRO: PSA self-test ok (AES-128-CBC + SHA-256)`.

To include the private JJWXC app, clone `cpyos-jjwxc` into `src/jjwxc` and add
`CPYOS_PLUS_JJWXC=1` to the configure environment (the build fails if the
directory is missing, and the source must be removed before any commit). The
verified build is 27 sources / +376,288 B; first hardware checks are the app
entry under **My → Tools**, the disclaimer, and that an offline cached book still
opens. Account/login paths are the same private code as Read Pico, so treat them
as untested until exercised on this board.

The read-only checklist can be driven automatically (it never flashes, reboots
or writes files) **only against the shell image with a UART0 console** (GPIO58/59
or a UART adapter), never the bring-up image over USB:

```bash
python3 scripts/bringup_serial.py --port /dev/cu.usbserial-XXXX --out /tmp/plus-bringup
# transcript: /tmp/plus-bringup/bringup.log, framebuffer: screen.bmp
```

It sends `info/batt/rtc/sd/imu/touch/keys/crypto/selftest/panel/window` and
`screenshot`, then reports any missing marker. Use the ESP-IDF or PlatformIO
Python (pyserial). The bring-up image instead shows the on-screen status and the
2 s `UI-PROBE alive ...` heartbeat described in §1a; it has no command console.

The serial console accepts commands (zero-timeout, never blocks the UI):

```
help info batt rtc sd imu touch keys crypto wifi light <0-100> [warm]
ap <ssid> [pass] | apoff | web [ssid] [pass] | weboff | usbmsc on|off
selftest | touchraw | haptic [ms] | rtcset <unix-seconds-utc> | bt on|off|scan [seconds]
```

### 3.1 `selftest`

Expected: a block with `chip=esp32s31 ...`, BQ27220 mV/mA/SOC plus a `charger:` line (VBUS type + charge phase), PCF8563 date,
`sd: mounted` plus a few root entries, `imu: available`, `touch: present=1`,
`pinyin: loaded=1 syllables=<n>`, `hyphen: en=1 bytes=<n> loaded=en`,
`crypto: ok`, `frontlight: present=0 warm=1` (**stub: the capability is off**;
`warm=1` only reports the configured second channel). The two data probes fail
(and print `loaded=0`) in the stock layout because the CPYOS data packs are not
written there; that is the expected degraded state, not a crash.

### 3.2 Panel + input

- `touchraw` prints ten raw samples. Touch the four corners and the three
  cover-key zones; confirm the raw coordinates land in the expected ranges
  (cover keys report raw_y >= 1570).
- Tapping the three screen thirds should page back / open settings / page
  forward in the reader; the top-right 56×60 corner toggles a bookmark.
- Two-finger pinch in the reader changes the font size by one step per gesture.
  Put two fingers on the page and spread/pinch by roughly 15 %; the serial log
  should show `pinch 1.x -> N pt`. If nothing fires, record whether both contacts
  appear (the chip's second point is read at 0x07..0x0A) and the raw sample from
  `touchraw`; do not adjust thresholds before confirming the second point exists.
- `keys` prints the raw TCA9555 inputs. Cover/volume keys are active low:
  **power = P1.7**, **volume previous = P1.0**, **volume next = P1.1**. If the
  order is reversed on your unit, swap the two mappings in
  `HalGPIO::update()`; if `power` never drops, check the expander interrupt.
- `slpinfo` prints the last wake cause, the GPIO2 (expander INT) level and whether
  the power key is held. Sleep is **not wired yet**; the vendor sequence is now
  documented step by step in engineering doc §2.7 (SD park → TPS/bus → INT release
  → key mask → P1.7 input/INT idle-high → EXT1 ANY_LOW + ≤60 s timer → rail cut +
  bus latch → `s31_sleep`). On the first hardware session, record `slpinfo` before
  and after a manual power wake from the vendor firmware, plus the TCA P0/P1 dump,
  to confirm wake-cause decoding and the P1.7 polarity before porting that path.
- `haptic 35` pulses the motor directly (TCA P0.3). If it stays on after the
  command, cut power and swap the level writes in `updateHapticFeedback()` —
  the polarity is not yet measured.

### 3.2.1 Panel modes and windowed refresh

- `panel` clears the frame and pushes a border + bar chart + 16-px line grid with
  a full **GC16**. The bars must be evenly spaced and the 1-px lines continuous;
  photograph it as the reference for later comparisons. `panel fast` now requests `FAST_REFRESH`, which maps to the
  differential GL16 waveform (FULL keeps GC16); expect a lighter, slightly
  ghosted result and a shorter flash. This is the on-device A/B for the mode
  mapping — if ghosts persist after the shared full-refresh cadence, record it.
- `window` fills a 220×140 box and calls `displayWindow()`. **Area refresh is
  intentionally disabled in this build** (the vendor never uses it on this panel
  class; partial scans are a gate-lock risk), so the call degrades to a full-frame
  GL16 push with a one-time `displayWindow: area refresh disabled` log. Verify the
  push completes and the panel does not lock; do not expect only the rectangle to
  change optical state. If the area path is re-enabled later, it needs a bounded
  scan watchdog and an on-glass validation. Log it and
  report the panel revision.
- `screenshot` writes `SCREENSHOT_START:<bytes>\n`, the raw BMP, then
  `SCREENSHOT_END`. Capture it without Wi-Fi:
  ```python
  import serial, glob, time
  s = serial.Serial(sorted(glob.glob('/dev/cu.usbmodem*'))[0], 115200, timeout=1)
  s.write(b'screenshot\n')
  buf = b''
  while b'SCREENSHOT_START:' not in buf: buf += s.read(4096)
  head, rest = buf.split(b'SCREENSHOT_START:', 1)
  size = int(rest.split(b'\n', 1)[0])
  data = rest.split(b'\n', 1)[1]
  while len(data) < size + 1: data += s.read(size + 1 - len(data))
  open('screen.bmp', 'wb').write(data[:size])
  ```
  (The HTTP `/api/screenshot.bmp` endpoint is the same image once Wi-Fi is up.)

### 3.3 IMU

Settings → 控制 → 体感翻页 → 摇晃, then shake left/right in the reader. The
serial log should show shake triggers. If nothing fires, record whether
`imu: available` was true; the axis mapping may need `imuSwapXY`/sign changes.

### 3.4 Frontlight — not enabled in this build

`FREEINK_CAP_FRONTLIGHT` is 0 for this port, so `FrontlightManager` is a stub:
`light 30 50` / `/api/frontlight` only update settings and GPIO6/7 stay dark.
Do **not** judge LED polarity from a dark panel. Before enabling the capability,
clamp UI 100 % to 10 % of PWM full scale (vendor `FL_OUTPUT_MAX_PERCENT`); the
shared manager otherwise commands full duty and can over-drive the LEDs. The
standby “LEDs off” behaviour is therefore not exercised either.

### 3.5 BLE page-turner

`bt scan 10` starts the BLE host and lists nearby peripherals (`[HID]` marks those
advertising 0x1812); the same host is what **Settings → Controls → Bluetooth**
drives (scan / paired list / forget / disconnect). In that page, scan and pair the
remote; the serial log should show `PLUS-BLE: host synced`, `HID report subscribed`, then key
events while pressing buttons. Keyboard boot reports and consumer
scan-prev/scan-next are supported; remotes that only emit custom report bytes may
produce nothing (raw button learning is not implemented on this port).

### 3.5.1 Calibre upload (WebSocket, port 81)

Start the transfer server (`web <ssid> [pass]` or join a network and use `web`),
then point the CrossPoint Calibre plugin at the device. The server implements the
documented contract on port 81:

```text
START:<filename>:<size>:<path> -> READY -> binary -> PROGRESS:<n>:<total> -> DONE
```

One upload at a time, bytes go to `<name>.part` and are renamed at completion;
disconnects/errors delete the partial file. If the plugin reports a protocol
error, capture the server log (`PLUS-WS`) — the error strings match the
documented table (`Invalid START format`, `Upload already in progress`, ...).

### 3.6 Storage paths

- UI path: **My → Tools → File Transfer** now opens the native transfer page
  (saved-network auto-connect or the Wi-Fi picker, address, Calibre instructions
  and live upload progress); Back stops the server and releases the radio.
- Wi-Fi: `web TestAP 12345678`; join the AP and open the printed URL. `apoff`/`radiooff`
  release the radio (`esp_wifi_deinit`) instead of leaving it powered. List /
  upload / download / delete under the SD root; `/.crosspoint` must be hidden.
- Documented API smoke test from a phone/laptop on the same network:
  ```bash
  curl -s http://<ip>/api/status        # version/ip/heap/battery/charger
  # browser: http://<ip>/ (official home), /files (Chinese; /files/en English),
  #          /fonts, /settings (read-only + frontlight), /simple (minimal)
    # fonts: http://<ip>/fonts (upload .ttf/.otf/.ttc to /fonts/<family>/; the registry
  #        is marked dirty, so the next Settings → Fonts entry rescans)
  #        http://<ip>/diag (version, reset/wake, heap, SD free, packs, touch, USB,
#          screenshot link)
  curl -s "http://<ip>/api/files?path=/"
  curl -o screen.bmp "http://<ip>/api/screenshot.bmp"   # live 1-bit framebuffer
  curl "http://<ip>/api/frontlight?brightness=30&warmth=50&on=1"
  curl -s http://<ip>/api/settings
  curl -s http://<ip>/api/diag          # reset/wake, heap, pinyin/hyphen loaded, touch, USB
  curl -OJ "http://<ip>/download?path=/Books/MyBook.epub"
  curl -F "file=@mybook.epub" "http://<ip>/upload?path=/Books"
  curl -d "name=Read&path=/" http://<ip>/mkdir
  curl -d "path=/Books/a.epub&name=b.epub" http://<ip>/rename
  curl -d "path=/Books/b.epub&dest=/Read" http://<ip>/move
  curl -d 'paths=["/Books/old.epub","/OldFolder"]' http://<ip>/delete
  ```
  `/upload` overwrites an existing name (as documented) and answers
  `File uploaded successfully: <name>`. A truncated body must return an error
  and leave no `.part` behind.
- USB from the UI: the File Transfer page has a **USB Drive** capsule; the first
  tap arms it, the second within 8 s hands the card to the host. The screen then
  freezes (the app must not touch storage) — power-cycle to return. The console
  equivalent is `usbmsc on`.
- USB: `usbmsc on`, then mount the new USB drive on the host. The console may
  disappear — this is expected. Power-cycle to return to normal firmware.

## 4. Recovery

- WebDAV: mount `http://<ip>/` from Finder/Windows/`cadaver`. Check
  `curl -X PROPFIND -H "Depth: 1" http://<ip>/` returns 207 with
  `<D:multistatus>`; PUT/DELETE/MKCOL must work in an existing folder. Writes use
  a temp file and rename; `/.crosspoint` and `..` are refused.
- **Power-cycle** restores normal storage after USB MSC (there is no remount path).
- **Reflash** with `flash_metalio_plus.py --layout vendor --slot both --firmware ...`
  for the stock layout (bring-up/reader), or `--layout port --full-install` for a
  CPYOS table. Never use `idf.py flash`: it writes this project's bootloader and
  partition table over the stock layout.
- **Full restore** = write the 16 MiB backup back at `0x0` with esptool
  (this is the fastest way back to the factory image).
- Never write a Read Pico image to this board.

## 5. Record results

For each check record: pass/fail, the serial lines, and (for anything visual) a
photo. The items most likely to need follow-up are VCOM, panel waveform/gamma,
touch corner mapping, IMU axes, and frontlight polarity.

## 1b. Flashing without a USB replug, and the console (2026-11-10)

**The board has no EN reset line.** esptool's `RTS->EN` reset does nothing, and once
the app owns the USB-Serial/JTAG peripheral a plain high-baud stub connect can fail
with `No serial data received`. Reference behaviour:

- vendor 2.0.74: UART0 console + USJ secondary; updates go through `main/ota.cc`
  (in-app OTA / SD firmware) and the web flasher, never esptool-while-running.
- kiikoread: **USB-Serial/JTAG console as the primary console**, updates through
  `ota_online.c` (`esp_ota_*`) and the GPIO36 USJ/UTMI route switch.

The port now does both:

1. **Console on USB-Serial/JTAG** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, secondary
   none). Logs and a line console are available over the same USB port.
2. **Replug-free flashing**: `scripts/flash_metalio_plus.py` first tries the normal
   high-baud connect; if it fails it syncs with
   `--baud 115200 --no-stub --after no-reset` and then runs every later command with
   `--before no-reset` (the chip stays in ROM download mode). Verified repeatedly on
   the running app.
3. **Device-side update** (`flash <sd path>` on the console, and Settings → System →
   **SD Card Firmware Update**) uses `firmware_flash::flashFromSdPath`: validates the
   image and writes the **inactive OTA slot** + otadata. This needs no USB at all and
   is the same model as the vendor and kiikoread.

Console commands for automated bring-up (over USB, no extra hardware):

```
info  batt rtc sd imu touch keys i2c slpinfo sdwrite haptic [ms] panel [fast]
window screenshot crypto pinyin hyphen wifi light <n> [warm n]
lang <zh|en>
tap <lx> <ly>            # synthetic tap, logical portrait 0..683 / 0..1215
key <back|ok|left|right|up|down|power>
flash <sd path>          # write the inactive OTA slot and reboot
web [ssid] [pass] | weboff | usbmsc on|off | bt on|off|scan
```

`exp/plus_console.py` wraps this (send commands and/or save the framebuffer BMP) so a
host script can walk the UI and capture screenshots.

### Boot-blocking bugs found on the way (all fixed)

1. **TLS overflow** — newlib's `localtime()` needs a 2,524-byte thread-local buffer,
   and the RISC-V port places a task's TLS block inside that task's stack. The 1 KB
   IPC task overflowed it and wiped the heap (`free=0`), so `esp_ipc_init` asserted
   and the device boot-looped. Fixed by using `localtime_r()`.
2. **`thread_local std::mt19937`** in the Arduino `random()` shim (2,500 bytes) was
   another TLS block in every task stack. Made it a plain `static`.
3. **ODR/layout mismatch** — `FontCacheManager`'s members depend on
   `ENABLE_CHINESE_VERSION`; `cpyos_ui` defined it privately while `main` did not, so
   the constructor wrote past the end of the (smaller) object allocated in
   `shell_demo` and zeroed the adjacent `activityManager` vptr. Every layout-affecting
   macro (`ENABLE_CHINESE_VERSION`, `CROSSPOINT_VECTOR_FONTS`,
   `CROSSPOINT_SYSTEM_FONT_PACK`, `CROSSPOINT_FONT_PACK_EMBEDDED`,
   `CROSSMUX_UI_PROFILE_HIGH_DPI`, `BOARD_HAS_PSRAM`, `USE_UTF8_LONG_NAMES`) is now
   **PUBLIC** on `cpyos_ui`.
4. **Unused `<iostream>`** pulled libstdc++'s `std::cin/cout` global constructor into
   every TU; removed (the serial shim writes to `stderr` with `fputc/fwrite`).
5. **Missing chrome font ids** — `BaseTheme::STATUS_NUMERIC_FONT_ID`,
   `READER_STATUS_FONT_ID`, `READER_ESTIMATE_FONT_ID` are now registered, removing the
   `Font -858375107 not found` fallback.
6. **Language** — the shell now calls `I18N.setLanguage(SETTINGS.language)` before
   `I18N.init()`, and a fresh Plus install defaults to Chinese
   (`CROSSPOINT_PLUS_PORT`).
7. **CFF/OTF glyph faults on the `main` task** — opening an EPUB with an embedded
   CFF font panicked with `Stack protection fault` in `cf2_interpT2CharString`
   (FreeType's Type-2 interpreter reserves a single 16,128-byte frame).
   `EpubReaderActivity::onEnter()` calls `face->build(" ")` through
   `loadEmbeddedFontIfPresent()` on the `main` task, whose 12,288-byte stack
   cannot hold it. The render task already has a 64 KB PSRAM stack, so this only
   hit glyph work done outside `render()`. Fixed by raising
   `CONFIG_ESP_MAIN_TASK_STACK_SIZE` to **32768** (`sdkconfig` and
   `sdkconfig.defaults`; costs ~20 KB internal RAM, still leaves ~137 KB free).
   After a `sdkconfig` change, run `idf.py reconfigure` before `build` — the
   env-gated components (`CPYOS_UI_PROBE`, `CPYOS_ACTIVITY_PROBE`,
   `CPYOS_SHELL_DEMO`, `CPYOS_PLUS_JJWXC`) are only re-evaluated at configure
   time, and a build without them re-creates an empty `cpyos_ui` and the
   "Logging.h not found" / "driver/gpio.h not found" wall. Verified on glass:
   `embedded-font-test.epub` now opens and renders with the embedded Source Han
   face, and the TOC/page turns work.

On glass: Home and Shelf render in Chinese, the type scale matches Read Pico's Large
default, and the bottom tab labels fit the screen.

## 1c. Product build composition (2026-11-10)

- **Private JJWXC component**: opt in with `CPYOS_PLUS_JJWXC=1` and a checkout at
  `src/jjwxc` (never committed). The component CMake already wires the sources,
  include dirs and `CROSSPOINT_BOOKSOURCE_JJWXC=1`.
- **Arduino `WebServer` compat**: `components/cpyos_ui/compat/WebServer.{h,cpp}`
  implements the small Arduino surface the JJWXC cookie-import page uses
  (on/onNotFound/begin/send/arg) on top of `esp_http_server`, so the import page
  works without the Arduino network shell.
- **Web pages are served gzip-compressed**: `scripts/gen_web_pages.py` emits gzip
  blobs (`kFilesPageZh`, … plus a `…Len`) and `web_transfer.cpp` answers with
  `Content-Encoding: gzip`. The two Files pages alone shrink from ~465 KB to
  ~80 KB of flash.
- **USB MSC leaves on its own**: the storage callback sees the host release the
  volume (`MOUNT_COMPLETE` back to the app mount point) and the shell reboots,
  so a transfer no longer strands the device in U-disk mode.

### Radio and book-source fixes found by the 2026-11-10 device sweep

All four were invisible until the corresponding screen was opened:

1. **NVS was never initialised.** `esp_wifi_init()` opens its own namespace and
   failed with `ESP_ERR_NVS_NOT_INITIALIZED`, so every scan returned -1 and the
   Wi-Fi picker said "连接失败 / 通用故障". `cpyos_shell_demo()` now calls
   `nvs_flash_init()` (erase+retry on `NO_FREE_PAGES` / `NEW_VERSION_FOUND`)
   before any radio use. `Preferences` remains lazy, so this is the only NVS
   init on the boot path.
2. **Async scan results were never collected.** `WifiSelectionActivity` starts
   the scan with `scanNetworks(true)` and polls `scanComplete()`; the compat
   layer returned the driver's count but left its `s_scan` vector empty, so the
   picker always showed "找到 0 个网络" even though `wifi` listed an AP. The
   WIFI_EVENT_SCAN_DONE handler now marks completion, and `scanComplete()` copies
   the AP records the way the synchronous path does. `WIFI_SCAN_RUNNING` and a
   genuinely empty scan stay distinguishable.
3. **`NetworkStartup::releaseRadio()` deinitialises the driver.** It calls
   `esp_wifi_deinit()` directly (Arduino's WiFi library re-initialises
   transparently); the compat layer cached `s_initialised` forever, so the first
   radio release (leaving the Wi-Fi sheet) killed Wi-Fi for the rest of the boot
   — `web <ssid>` could not start a SoftAP afterwards. `ensureDriver()` now
   detects `esp_wifi_get_mode()` failing and re-creates only the driver (the
   netifs and event loop survive deinit). Verified with a SoftAP + HTTP server
   start and a `GET /` that returned 200/2104 bytes.
4. **The private book source was compiled as the null stub.** `BookSource.cpp`
   lives in `cpyos_ui`, but only `cpyos_app` defined
   `CROSSPOINT_BOOKSOURCE_JJWXC`; moreover `JjwxcSource.cpp` (the root-level
   `ISource` adapter) was not in the component's glob, and the final link scans
   `cpyos_ui` after `cpyos_app`, so the reference would have been unresolvable.
   Fixed by compiling `JjwxcSource.cpp`, defining the macro for `cpyos_ui` when
   `CPYOS_PLUS_JJWXC` is set, and anchoring `booksource::privateSource()` from
   `shell_demo` (a main-object reference keeps the archive member from being
   skipped). The 晋江 tile now opens the disclaimer and the app home instead of
   doing nothing.
- Sizes (7.19 MiB OTA slot): shell without JJWXC 7,093,920 B; with JJWXC
  7,293,920 B (242,720 B free).

5. **WeRead crashed in FreeType's auto-hinter.** Launching the app (after its
   disclaimer file check) faulted in `af_latin_hints_compute_segments`
   (`prev_segment->last` was NULL, `Load access fault` at address 2). The
   auto-hinter is not required for the 4-bit coverage path or the 50% BW
   quantizer, so `components/freeink_font` now builds with
   `FREEINK_FONT_ENABLE_AUTOHINT=0` on this target; WeRead opens and its text
   looks identical to the hinted UI in side-by-side captures. Re-enable only
   with a fixed autofit and a device re-test.
6. **Unguarded TLS verification blocked every JJWXC request.** Wi-Fi associated
   and got an IP, then `esp-x509-crt-bundle` rejected the chain with
   `PSA signature verification failed (-141)`. The reference build calls
   `SecureClient::setInsecure()`, and the non-wolfSSL `esp_http_client` fallback
   in `WeReadHttpClient` unconditionally attached the bundle. That attachment is
   gone, and `CONFIG_ESP_TLS_INSECURE/…_SKIP_SERVER_CERT_VERIFY` let esp-tls run
   without a CA (the reference policy: no peer authentication). The request now
   reaches the body-read phase; AP-dependent flows still need a live network.
7. **Band-diff rendering is off on this port.** `displayWindow` degrades area
   updates to a full-frame scan, so a band-only repaint would publish stale
   pixels outside the band. `CROSSPOINT_PORT_NO_BAND_DIFF` makes
   `GfxRenderer::setBandUpdates()` a no-op here.
8. **`screenshotgray` shows the truth on 16-level pages.** `CMD:SCREENSHOT`
   reads the 1-bit proxy, whose untouched areas keep the previous page; the new
   command dumps the retained 4-bit canvas as an 8-bit BMP (832,822 bytes).
   Keep in mind a 1-bit page leaves that canvas stale, so use the proxy for
   1-bit pages and `screenshotgray` for grayscale ones.

### Online bring-up fixes (2026-11-10, later round)

- **mbedTLS/PSA ran out of internal RAM.** A handshake bottomed the internal heap
  at ~3.5 KB and PSA returned `INSUFFICIENT_MEMORY` (-141); PSRAM had 12 MB free.
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` moves mbedTLS/PSA allocations to PSRAM
  (the same policy the reference build applies to wolfSSL).
- **`SecureClient` had no TLS deadline.** It read `_transport.getTimeout()`, which
  the callers never set, so `esp_tls` used no timeout and a stalled handshake hung
  forever. It now uses the client's own `getTimeout()` with a 30 s floor.
- **`SecureClient::available()` never made progress.** esp_tls only decrypts socket
  bytes during `read()`, but `SecureHttpClient::readLine()` polls `available()`.
  The port now keeps a 512-byte receive window and pre-reads when `select()` says
  the socket is readable; this is what made the browser work (it rendered
  `m.jjwxc.net` as 24 pages and `example.com` as one page).
- **Activity logs were compiled out.** `cpyos_app` now defines
  `ENABLE_SERIAL_LOG` + `LOG_LEVEL=2`; without it the browser/JJWXC `LOG_*` calls
  were empty, which is why their failures were invisible. Also added the missing
  Arduino `BSSID(uint8_t*)` overload that only surfaced once logs compiled.
- **The shell loop starved IDLE-priority workers.** It now `vTaskDelay(1)` per
  iteration; the browser fetch task (IDLE priority, core 0) previously never ran.
- **A stuck fetch task must not be deleted mid-heap.** `onExit` waits 35 s; past
  that the Plus restarts (`CROSSPOINT_PORT_STUCK_JOB_RESTART`) instead of deleting
  a task that may still hold heap/TLS state. Replacing the browser *while a fetch
  is finishing* therefore reboots — avoid it in automation by starting the
  browser once per boot.
- **Book-detail hero layout** now derives meta/title/author/tagline positions from
  the measured line heights; the old fixed 26/68/92 px offsets assumed a 24 px
  title and printed it over the author on the Large scale.

Verified on glass: JJWXC discover (`古言` book list), book detail (cover
placeholder, 143 chapters, facts, buttons), browser pages over HTTPS from both
`m.jjwxc.net` and `example.com`. Still pending (needs an account decision):
JJWXC login/cookie import, shelf, caching and comments; WeRead data.

### JJWXC session import and end-to-end reading (2026-11-10, verified)

- **Session jar written over the console.** The Mac and the device sit on
  different subnets, so the import page was unreachable. `CMD:PUTFILE` writes a
  file 64 bytes per chunk with an ACK per chunk (larger CDC writes lose bytes).
  The jar is the device format: `name\tvalue\tdomain\tpath\tsecure\texpires\thostOnly`
  lines, XOR'd with the **Wi-Fi STA MAC** (`esp_read_mac(ESP_MAC_WIFI_STA)`,
  logged as `sta (1c:29:04:d1:50:16)`) — *not* the eFuse base MAC esptool prints —
  then base64. Two more traps: `Cookie::domain` must have **no leading dot**
  (`jjwxc.net`, not `.jjwxc.net`), and `JJSESS` exceeds `kValueCap` (546 > 512) so
  it is dropped; `sid` alone authenticates WAP.
- **Verified end to end**: session restored (`6 cookies, readerId=17289484`),
  authenticated `/my/fav` (143 books over 5 pages, GBK), book detail, chapter
  list (54 entries), caching chapter 1 through the App API, EPUB packaging into
  `/JJWXC/绅士的秘密-10982409.epub`, and the reader showing the chapter plus the
  App API `sayBody` under 「作者有话说」. Paging works; 16-level text.
- **Open issues found in this pass**:
  1. Cover thumbnail conversion failed with `sdmmc_cmd: allocate_dma_buf: not
     enough mem` while packaging (internal RAM is tight there). The cover itself
     downloaded (328 KB); only the BMP thumbnail failed.
  2a. Opening the downloaded book from 已下载 and then tapping 章节列表 can leave
     the detail page blank (header only) while a job is queued/running: the log
     shows `job task up` but no HTTP line for >20 s, the device is idle, and
     Back/re-enter repopulates it. The page clears without a wait card. Tracked
     as the blank-BookDetail issue; suspect the job state vs render path in
     JjwxcActivity rather than layout.
  2. The classic reader menu (`EpubReaderMenuActivity`) and the Contents panel
     draw their list backgrounds/separators/scrollbar inside ~480 px while row
     values sit at the right edge (~600), and the header title/progress band is
     partly missing. Evidence: `artifacts/plus/read9_menu.png`,
     `read10_menu2.png`, `read11_contents.png`. Same family as the Display
     settings tab/card overlap — the fui list layout appears to use a width that
     disagrees with the 684 px screen for these legacy-style screens.

Known UI issue (open): the Display settings category draws its tab strip and
group card overlapping the header when the list rebuilds (arts:
`artifacts/plus/z8_reopen.png`, `z12_reopen2.png`); System/Reader/Controls are
clean. The tab-band probe reported one build at `45,112 594x72` with content
`5,192 674x1016`, so the duplicate is drawn by the list rebuild path, not by a
second activity; needs a focused follow-up.

Feature sweep on device (2026-11-10). Verified end to end with screenshots:
Gomoku (menu, resume entry, 15x15 board, stone placement, counters, timer),
2048 (4x4 grid, tile dithers, swipe move, new-tile spawn), Sokoban (level
1/155, board, swipe move updates the step counter), Minesweeper (difficulty
menu, 9x9 board, dig reveals the flood-filled numbers), standby clock screen
(SloppyClock with sleeping duck, date, battery), library grid, About page,
reading-stats page, Settings landing/System/Reader/Display pages, app
visibility list (enabling Sokoban/Minesweeper/2048 adds them to the tools
grid), language switch ZH<->EN, and the Chinese pinyin IME (globe key
activates, "niha" shows the candidate strip with the first item inverted,
tapping commits the character into the search field; `CMD:pinyin` reports
loaded=1 syllables=425). The My dashboard tab bar no longer uses a
fixed 120 px pitch: `MyActivity::tabRect()` sizes each slot from the measured
label (bold when active), so the English labels ("Records") no longer run
into the next tab; the CJK rhythm is unchanged (64+56=120). **Only English and Simplified Chinese are
available**: the port has no i18n data partition and the 334 KB 35-language
pack does not fit the ~160 KB of app0 headroom; the remaining 33 languages
would need a data partition (a release-layout change).

Classic header titles (2026-11-10, fixed). The reader menu and the chapter
selection screen drew their back arrow, battery and progress line but no
title; `GfxRenderer` logged `Font -1094390662 not found`. That id is
`NOTOSERIF_TITLE_FONT_ID`: main.cpp registers the built-in Song serif for it,
and this port's `registerFonts()` never did. The bitmap serif is ~190 KB and
does not fit the remaining slot headroom (the image went 43 KB over app0), so
the port now maps the id onto the system pack's title face
(`setPreferredFont(NOTOSERIF_TITLE_FONT_ID, CJK_UI_12_FONT_ID)` plus the 8pt
fallback) after `typography::apply()`. Titles are visible; they render in the
vector sans title face instead of the embedded serif.

Toolbar reader validated (2026-11-10). With Reader settings -> Reader menu style
= Toolbar, a centre tap opens the CPY toolbar (Contents / Text / Bookmarks /
More) over the live page, with the page slider and chapter/page footer. The
Contents panel shows the All/Cached/Uncached tabs, the fixed "Update chapter
list" action bar, ordinal-prefixed rows ("1. 第 1 章"), FREE/VIP badges,
download buttons and the current-chapter bar. The Text panel shows the font
picker row, size/line-spacing/paragraph/margin steppers, the fast-turn and
water-ripple toggles and the reset button. Note the panel is painted
synchronously from the tap handler and the panel scan adds ~1 s on top, so a
screenshot taken <1.5 s after the tap can still show the bare page; the tap is
not being dropped. The Bookmarks panel shows 书签(N)/批注(N) tabs, the fixed
add/export action bar, the bookmark rows with page excerpts and delete buttons,
and the empty "暂无批注" state; the More panel shows 书签 / 添加书签 / 深色模式 /
查词 / 阅读方向 / 自动翻页（每分钟页数）/ 跳转到 % and scrolls.
Also verified offline: the Wi-Fi selection bottom sheet (scan result with lock
and chevron, "添加隐藏网络…", 重试/取消 action bar) and the Update page
(current version 0.0.0-plus-untested, Nightly toggle off).

Bookmark flows verified (2026-11-10): the classic menu's bookmark list shows
name/excerpt plus page and progress, long-press opens 打开 / 重命名 / 删除,
rename uses the numeric/text keyboard and the new name persists, 删除 asks
"删除此书签?" with 取消 focused, and deleting returns to the menu where the
书签 row disappears. After the embedded-font mitigation the text-settings
preview is clean again (two well-spaced sample lines), which also confirms the
garble came from the embedded face, not the preview layout.

EPUB-embedded fonts (readpico's A5) are skipped on this port (2026-11-10,
workaround). A book that embeds an OTF/CFF face rendered with overlapping
lines: the body glyphs came out at roughly twice their nominal pixel size while
the face's line metrics stayed at the nominal size (preview logged lineH=44 /
advance=51 at 18 pt, i.e. the same pitch as the working system-pack pages,
while the ink spanned ~2x that). The embedded test book uses the SHSansSC OTF;
the same family supplied through the system pack lays out correctly, so the
suspect is the CFF path's units-per-em scaling, not the book. Until that is
fixed, `loadEmbeddedFontIfPresent()` returns early under
`CROSSPOINT_PLUS_PORT` (log: "embedded EPUB font ignored on this port") and the
book reads with the normal reader face - verified: the test book now shows a
single correctly paginated page. Side observation: the reader's 清除分页缓存
action did not evict that book's section cache (the next open still logged
"Cache found"), so a stale layout built under different font metrics can
survive it.

Held-touch injection (2026-11-10). `touchdown <lx> <ly>` / `touchup` console
commands keep a synthetic contact down across frames, so the port's 500 ms
long-press timer fires (the plain `tap` injection is down+up in one frame).
Verified on device: library long-press opens the book action menu (打开 / 删除 /
刷新书架索引) and rebuild runs; reader long-press selects a word and shows the
高亮 / 批注内容 chips, and 高亮 applies and persists the highlight in the page.

Wi-Fi scan must never block forever (2026-11-10). `WiFiClass::scanNetworks()`
used `esp_wifi_scan_start(..., block=true)`, which waits indefinitely when the
SCAN_DONE event is lost (radio released mid-scan, driver wedged). One such
scan hung the console task: every later command (`info`, `screenshot`) produced
no output and the only way back was a USB flash (esptool still works because it
resets into download mode). The wrapper now starts the scan non-blocking and
waits for `s_scanDone` with an 8 s deadline (`scan timed out after 8 s`), so the
console task and the Wi-Fi page cannot wedge.

SD wedge + console reboot (2026-11-10). After a long session the SDMMC host
started failing every write (`sdmmc_host_do_transaction failed`, `0x107`
timeout) while `sd` still reported "mounted"; opening a book then showed the
index-failure card (`索引失败：图书无效`) rendered as **tofu boxes** and Back was
deferred behind a busy render, i.e. the UI wedged. There is no reset key on
this board, so a new console command `reboot` (esp_restart) was added; after
it the card mounted and listed normally and the same book opened with the
author's note intact. Follow-ups: (a) treat SD write errors as recoverable
(offer a remount/retry instead of a modal that swallows Back), (b) the
index-failure card title is drawn BOLD through the vector UI_10 family and came
out as tofu boxes even though regular CJK text on the same screen resolved
correctly - check the vector family's bold face when the font system is under
pressure.

Display-category forensics (2026-11-10, still open). **CJK-only**: with the
UI in English the same page renders perfectly clean, so the extra ink is tied
to the CJK fallback face resolution, not to list geometry. Switching the label
style from `smallText` to `bodyText` changes nothing (both resolve to the same
effective CJK face), so the label style is not the trigger.
 The duplicate is not a
second *activity*: in one `UiListActivity::render` frame every label is drawn
exactly once (text/glyph/render/blit probes all agree, e.g. `待机画面` at
`x=121 y=255`, glyph tops 25/25/23/23, blit outer 264/266), yet the framebuffer
ends up with an extra copy of the **first one or two characters** of each label
about 24 px lower (`待机` at y 288..314, `画面` at 266..290). `fbclear` (new
console command) zeroes the framebuffer, and re-rendering the page reproduces
the same ink count (53,354) every time, so the extra pixels are produced by the
single render rather than left over from an older frame. The same page painted
as the background of an option popup is clean, and opening/closing the popup
flips between clean and dirty, so the difference is in which render path draws
it. Only the Display category shows it; Reader/System/Controls render clean.
Console helpers kept for the follow-up: `fbink <y0> <y1>` (dark-pixel count in
a physical-row band), `fbclear`, `screenshotgray`.



Internal DMA exhaustion: TLS handshake and SD transfers (2026-11-10, fixed)
The JJWXC shelf worked on a warm Wi-Fi session but a chapter cache crashed with
`mbedtls_ssl_handshake returned -0x7F80`, and opening a book detail failed its
cover with `sdmmc_cmd: allocate_dma_buf: not enough mem`. `CMD:dmaheap` showed
the cause is not "internal RAM is low" but that the **DMA-capable slice of
internal RAM** is tiny while Wi-Fi is up: `internal free=29 KB largest=24 KB`
but `dma free=4.1 KB largest=928 B` (at boot it is 100 KB/63 KB, and the panel
driver alone takes 90 KB at init). Three consumers allocate per-operation from
that pool on this part:

* esp-aes (mbedTLS hardware AES) takes DMA bounce buffers (8 KB in + 8 KB out)
  for every PSRAM-backed TLS record, because `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`
  puts the record buffers in PSRAM.
* esp_sha's PSA driver allocates each multipart hash context (104 B) and the
  HMAC opad with `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`. The measured failure was
  `ssl_tls.c: psa_hash_setup() returned -141` (PSA_ERROR_INSUFFICIENT_MEMORY) at
  ServerKeyExchange, which mbedTLS collapses to
  MBEDTLS_ERR_SSL_HW_ACCEL_FAILED. The server's certificate is RSA, so this is
  hit on every handshake.
* sdmmc's unaligned-buffer path allocates up to 8 KB per FS transfer (FatFS
  sector buffers are not 64-byte aligned).

Fixes (all verified on the device; shelf + cover + single-chapter cache +
comment counts/bodies + repackage + reader hand-off now run with 0 errors):

1. `CONFIG_MBEDTLS_HARDWARE_AES=n` and `CONFIG_MBEDTLS_HARDWARE_GCM=n` — software
   AES/GCM. ESP-IDF's own Kconfig note says hardware AES gives no speed boost at
   high CPU frequency; the port runs at 320 MHz.
2. `CONFIG_MBEDTLS_HARDWARE_SHA=n` — software SHA/HMAC, which removes every
   per-record internal allocation. A 16 KB record hashes in ~1 ms at 320 MHz.
3. The SD bounce buffer is now a **64-byte aligned 8 KB PSRAM buffer**
   (`cpyos_sd.c`: `host.dma_aligned_buffer`). SOC_SDMMC_PSRAM_DMA_CAPABLE is 1,
   the driver skips its alignment check for a caller-provided buffer and still
   runs `esp_cache_msync`, so aligned PSRAM DMA is valid and costs nothing from
   the internal pool. Verified by `cover thumbnail built: 200x280` and no
   `allocate_dma_buf` failures under TLS.
Diagnostics kept: `CMD:dmaheap` (free/largest per cap + probe allocations +
sd bounce pointer) and `CMD:cryptotest` (PSA GCM/HMAC/TLS12-PRF/ECDH statuses).

Silent restart into the reader (2026-11-10, fixed)
`JjwxcActivity::handOffToReader()` deliberately reboots after caching a chapter:
that is what reclaims the Wi-Fi heap for the reader. The reference build carries
the destination in an RTC flag; this port mapped all `silentRestartTo*()` to a
plain reboot, so "缓存并阅读本章" landed on Home and looked like a crash
(`rst:0xc`, reset=3). The port now writes a one-shot
`/.crosspoint/silent_target` (`reader\n<epub path>`) in
`silentRestartToReader*()`, and `cpyos_shell_demo()` consumes it after
`activityManager.begin()` and calls `goToReader(path)` (falling back to Home if
the file vanished or the reader failed to start). Verified end to end: cache ->
repackage -> reboot -> `silent restart -> reader ...` -> EpubReader at the
cached chapter.

Console `wificonnect <ssid> <password>` (2026-11-10)
`wifi` only scans. The new command stores the credential through the shared
`WIFI_STORE` (same `/.crosspoint/wifi.json` the Wi-Fi sheet writes, so later
silent reconnects work) and connects immediately; it prints the SSID, status and
IP but never the password. Used to move the unit to the replacement AP without
walking the on-screen keyboard.

Boot bring-up cycles are diagnostics only (2026-11-10)
`panel_color_test()` + `draw_bringup_pattern()` ran on every boot (2x white/black
GC16, ~12 s) including the JJWXC silent restart. They are now compiled only for
the BSP-only image (`!CPYOS_SHELL_DEMO && !CPYOS_READER_DEMO &&
!CPYOS_UI_PROBE`); UI builds paint a real first frame instead.

Author's note with no text (2026-11-10, fixed)
Chapters whose note is only whitespace (empty `note_str`, or one made of
`<br>`/NBSP) used to write the `作者有话说` heading and then the chapter text.
`JjwxcActivity::onChapterSay()` now skips a leading whitespace-only chunk before
starting the note section.

Storage free space was unreadable (2026-11-10, fixed)
The About/Storage row showed `--` and the dictionary installer refused with
"SD 卡空间不足" on a card with 15 GB free: `HalStorage::getSpace()` only used
`statvfs()`, which this FATFS VFS mount does not answer. It now prefers IDF's
`esp_vfs_fat_info("/sdcard", &total, &free)` (the FatFS-native query) and keeps
statvfs as the fallback. Verified: `CMD:sd` prints `space: 15066.3 / 15196.0 MB
free` and My -> System -> 存储 shows `15.8 GB`.

Web file transfer and browser verified online (2026-11-10)
With Wi-Fi up (`CMD:web`), a Mac on the same LAN used the device server:
`GET /` -> 200 (gzip page, 6,166 B decoded), `GET /api/list?path=/` -> JSON
listing with sizes, `POST /api/upload?path=/plus_upload_test.txt` -> 201 and the
file appears in the listing, `DELETE /api/file?...` -> 200. This also exercises
the SD write path through the PSRAM bounce buffer.
The browser app fetched `https://example.com` with software AES/SHA:
`GET attempt 1 status=200 ... in 2903 ms`, `fetch done result=0 http=200`, and
the page painted (title, URL, body, menu pill, 1/1).

Known gap: PDF is not built on this port
The image contains EPUB/TXT/FB2/XTC/CBZ and images, but no MuPDF sources; the
readpico PDF reader cannot run here without porting MuPDF to esp32s31 (separate
toolchain work). Tracked as a parity gap, not a regression.

SD write timed out while Wi-Fi was being torn down (2026-11-10, fixed)
Leaving the comment panel (which releases the radio) and letting the reader
rebuild its section failed reproducibly:
`sd_host_wait_for_event returned 0x107` on CMD24 (write), then
`diskio_sdmmc: sdmmc_read_blocks failed (0x107)` and
`[ERS] Build failed: spine=1 ...` -> the reader showed "索引失败：图书无效".
`esp_wifi_deinit` can hold the CPU for over a second, and the SDMMC host's
default command timeout is ~100 ms, so a transaction straddling that window
times out. `cpyos_sd.c` now sets `host.command_timeout_ms = 2000` at mount.
Verified with the exact repro (open the reader, open 章评, Back): the page
re-rendered after the pop with no 0x107 and no index failure.

Reader menu / chapter comments verified on the Plus (2026-11-10)
The classic reader menu scrolls (swipe up) and its second page carries the
JJWXC rows: 云端进度 / 更新当前章 / 更新章节列表 / 章评 / 书籍信息 / 清除分页缓存.
章评 fetched chapter 2 live: `http=200 bytes=26983 request=1898ms`,
`comment list: 20 shown / 247 total`, comment bodies cached, and the panel
painted 247 条 with like/reply pills. `[ERS] JJWXC book: novelId=10982409`
confirms the path parser works with this VFS.

Display-category label split: measured geometry (2026-11-10, still open)
A single label is painted as two runs with different faces: on "待机画面" the
first two glyphs sit at x 122..178 (28 px tall, y 285..314) and the rest at
x 182..233 with a visibly higher top (its glyphs start above y 285), i.e. the
second run is ~60 px to the right and uses a larger face. The same split lands
after exactly two characters on every row of this page (休眠|画面封面模式,
隐藏|电量百分比, 书库|布局 ...), while the Reader/Controls/System categories and
the Settings overview render the same strings cleanly. Both `cn_i18n_chars.txt`
(803) and `cn_common_chars.txt` (3517) contain every glyph involved, so this is
not a coverage fallback: the label is being drawn in two calls/runs with
different fonts and advances. A `CMD:texttest` probe that drew
`drawText(UI_12/UI_10, ...)` on a blank frame was tried and **removed**: calling
`clearScreen()` + `displayBuffer()` from the console task re-enumerates USB /
resets the unit (the panel driver is owned by the render task), so off-task
drawing is not a valid probe. Any future check must go through an Activity's
render path.

Serial opens were resetting the unit (2026-11-10) — automation pitfall
pyserial's default open asserts DTR and RTS, which the ESP32-S31 USB-Serial/JTAG
unit interprets as its reset sequence. Every `plus_console.py` / `plus_drive.py`
run therefore rebooted the device at connect (the log at capture start is always
a boot, and Home lost its recent card), and running two commands in separate
processes reset it twice. Both helpers now set `dtr = False, rts = False` before
`open()`. A single session then runs with exactly one boot (at connect) and no
further resets; keep multi-step flows inside one session.

ActivityManager is loop-task only (2026-11-10)
A console command that called `activityManager.pushActivity()` from the console
task entered the activity and then reset the unit (no coredump: a non-panic
restart). Console hooks must set a flag that `cpyos_shell_loop()` consumes and
performs the push itself. This is why the temporary `textprobe` activity was
removed after diagnosis instead of being kept.

Display-category split is not stale ink (2026-11-10)
Clearing between list rebuild passes (cancelling and re-beginning the gray
canvas) did not change the artifact, and the page clears its 1-bit framebuffer
on every pass; the two-piece labels are therefore painted within one frame, not
left over from an earlier one. Reverted that experiment. Next probe must go
through an Activity's render path (see the ActivityManager rule above) and log
the label/x/font/style at the draw site.

Parity matrix vs the readpico CPYOS image (2026-11-10)
Status is device-verified unless marked otherwise. "Verified" means an automated
tap/log/screenshot round on this unit, not just a build.

| Area | Status | Evidence / gap |
|---|---|---|
| EPUB reader (layout, 16-level AA, TOC, bookmarks, long-press selection, dictionary picker) | verified | page renders 1.3-2.3 s, bookmarks add/remove, selection highlight, 章评 round trip |
| TXT reading | verified | earlier round; TXT opened and paged |
| FB2 / XTC / CBZ / TXT | linked, **unverified** | ELF symbol audit: `Fb2` 62, `CbzReaderActivity` 20, `XtcReaderActivity` 27, `Txt` 98; test files for the next session are in `/tmp/plus_media/` |
| PDF | **not built** | MuPDF is prebuilt for xtensa only (`patches/mupdf/prebuilt`); the Plus would need a RISC-V MuPDF build + its ~1.7 MB, a separate multi-day port |
| Library grid/list, search, pinyin input | verified | pinyin IME candidates, library search |
| Reader toolbar + classic menu (Contents, bookmarks, text settings, More) | verified | all four panels; second page carries the JJWXC rows |
| Water-ripple page turn | **not available** | readpico drives E0470 waveforms directly; the Plus `HalDisplay::setPageTurnDirection` is a no-op (panel driver has no turn engine) |
| Fast page turn (B/W) | built (setting exists) | not specifically measured on the Plus; normal 16-level text is what the screenshots show |
| JJWXC shelf/detail/chapter list/search/discover/favorites/cloud progress | verified | 143-book shelf, 247-comment chapter, progress push |
| JJWXC single-chapter cache + repackage + silent-restart into reader | verified | packaged 54 chapters, `silent restart -> reader`, reader opens the cached chapter |
| JJWXC author's note | verified | empty-note heading fixed and re-checked |
| JJWXC purchase | **unverified** | needs the user's payment password; price page/auto-buy paths built |
| JJWXC comment like/reply/post | **unverified** | write paths exist; needs explicit user authorization per account-risk rules |
| WeRead app | built, **unverified** | login requires a WeRead account/scan |
| Web browser | verified | `https://example.com` 200/2.9 s, page painted |
| Web file transfer (HTTP API) | verified | list 200, upload 201, delete 200 from the Mac |
| OPDS / KOReader sync | built, **unverified** | need a server/account |
| SD card missing at boot | verified | port now routes to the shared `FullScreenMessage` page with the `存储卡错误` / `无法读取存储卡，请重新插拔后重试。` copy and a retry pill; retry re-runs the mount ladder through `Storage.begin()` (the port's `begin()` now really remounts) and stays on the page when it fails (`SD card retry failed; staying on the error screen`). |
| Settings, themes, language pack, system/SD fonts, emoji | verified except one page | Display category paints each CJK label as two differently-sized runs (see above); every other category and the overview are clean |
| Reading stats / games / calculator / standby overlay | verified | stats page, games, standby screen |
| Deep sleep / light sleep / wake | **not ported** | the port only has the sleep *overlay*; PMU/host sleep handoff is a separate task |
| OTA | **disabled** | port builds with `CPYOS_OTA_DISABLED=1`; device-side `flash <path>` writes the inactive slot instead |
| BLE page-turner | built, **unverified** | needs a BLE HID device |
| USB mass storage | console only | `UsbDriveActivity` has **0** symbols; the port stock console `usbmsc on` works but there is no UI entry (parity gap) |

Holding the CDC across a reset can wedge the USB-Serial/JTAG unit (2026-11-10)
Two units of evidence: every capture that stayed open across a `silentRestart`
or `esp_restart` ended with `device reports readiness to read but returned no
data`, and the unit finally stopped answering both the console and esptool
(0x0 bytes; the USB device still enumerates as "USB JTAG/serial debug unit";
no coredump, so not a panic). The same image had booted and run a full session
minutes earlier, so this is a host/USB-JTAG state, not a firmware regression.
Recovery needs a physical power press (no reset key on this board).
Rules for automation:
  * after flashing, wait for the boot banner/first frame before opening the port;
  * close the port before any action that reboots (cache-and-read, network
    activity exits), then reopen once the boot settles;
  * never open the port while the boot log is still streaming.


SD mount ladder was single-shot before this round (2026-11-11)
Symptom: `Library` reported `index build failed` and the shelf stayed at "卡上没有找到书"
even though files were on the card, and every POSIX call from the console
(`stat`, `open`, `mkdir`) failed with ENOENT. Root cause was not the filesystem
API: the card had failed to mount, and after that first failure the ladder could
never recover.

* `esp_vfs_fat_sdmmc_mount()` leaves the SDMMC host initialised when it fails,
  so every later profile in the ladder returned `ESP_ERR_INVALID_STATE` instead
  of retrying at a slower clock (measured: 40 MHz -> INVALID_CRC, then
  20/10/5/1 MHz and both 1-bit profiles all INVALID_STATE, all within 8 ms).
  The loop now calls `sdmmc_host_deinit()` between attempts and waits 20 ms, so
  a slow card really does step down.
* `host.flags` was overwritten with just the bus-width bit, dropping the
  driver's own flags (including `SDMMC_HOST_FLAG_DEINIT_ARG` on IDF 6.x); it is
  now OR-ed onto the default.
* Two sweeps: the second one runs without the PSRAM bounce buffer, so a
  DMA/coherency problem and a dead card are distinguishable in the log
  (`(sweep 1)` / `(no bounce)`).
* `MetalioSd_ReleaseHold()` now gets 20 ms to settle before the first attempt.

After the fix the log shows genuine per-profile retries, but on the current
hardware state *every* profile fails with `ESP_ERR_INVALID_CRC`, down to 1-bit
at 1 MHz and with no bounce buffer - i.e. the card is not answering at all.
The same image mounted fine at 40 MHz earlier in the same session (several
boots), so this looks like a card/contact or card-power state, not the driver.
Recovery needs the card reseated (or a real power cycle): a USB reset does not
power-cycle the card. Everything SD-backed (library index, settings file,
JJWXC cache, reader) stays unavailable until the card answers.

Card-side consequences seen while the card was missing, useful as a diagnostic:
the system font pack falls back to the embedded copy (no `system_font_*.dat`),
`space: unknown (getSpace failed)`, and `/.crosspoint` cannot be created because
there is no filesystem under `/sdcard`.

Media upload path that does work (card present): the console `putfile` with
64-byte chunks + per-chunk ACK; `exp/plus_putfile.py` drives it. Four test files
(TXT/FB2/CBZ/PDF) were uploaded this way; the library did not index them
because of the mount failure above, not because of the formats.


MSC is not an alternate path to a dead card (2026-11-11)
`usbmsc on` refuses with "no mounted SD card to expose", so the U-disk cannot be
used to check the card from the Mac while SDMMC fails - MSC only hands an
already-mounted card over. The U-disk entry itself does exist in the UI (the
transfer page owns it: `plus_transfer::usbStart()` -> `usb_msc::start()`), so the
"console only" line in the earlier matrix was stale; it just cannot start
without a card.

`info` build stamp can lag the running image (2026-11-11)
`info` printed `built=Nov 10 2026 20:40:43` while the log proved the newest code
was running (`(sweep 1)` only exists in the new SD ladder). Treat the printed
build time as an `esp_app_desc` artefact, not as evidence about which image is
in the slot; use a unique string from the change instead.


SD error page reached through the port's own mount (2026-11-11)
Read Pico turns a card that failed to mount into a retryable error page; the port
used to continue booting with defaults, which read as "the card has no books".
Now:

* `HalStorage::begin()` (port) calls `cpyos_sd_is_mounted()`/`cpyos_sd_mount()`
  first, so the shared retry button's `Storage.begin()` really remounts.
  Without that the retry could never succeed (it only re-created directories
  under an unmounted VFS).
* The boot route shows `goToFullScreenMessage(STR_SD_CARD_ERROR, BOLD,
  STR_SD_CARD_ERROR_DETAIL, retryable)` and returns instead of starting Home
  when no card is mounted.
* Console `retrysd` re-runs the mount without a reboot.
* Verified on hardware with a genuinely dead card: the page paints
  (`Entering activity: FullScreenMessage`, render 350 ms), tapping 重试 logs
  `[MAIN] SD card retry requested`, runs the full ladder, and ends with
  `[MAIN] SD card retry failed; staying on the error screen`.
* A failed mount is now released through `host.deinit_p(host.slot)` rather than
  the legacy `sdmmc_host_deinit()`: the latter hits the new SD driver's unset
  state and logged `SD_HOST: invalid argument: null pointer` per attempt
  without releasing anything.

USB-Serial/JTAG wedged a third time (2026-11-11)
After roughly twenty open/reset cycles in one session the console went silent
(0 bytes on open, `esptool ... chip-id` -> "No serial data received") while the
USB device still enumerated and no coredump appeared. Same signature as the two
earlier wedges; recovery is a physical power press. Automation now fails fast
with that message instead of hanging (`exp/plus_session.py`), and the rule from
the earlier note still applies: batch many steps into one session and do not
re-open the port while a boot is streaming.


Next hardware session: exact order (written 2026-11-11 while the unit was wedged)

Preconditions: unit power-cycled (press power once) and the microSD reseated.
Open the console ONCE and keep the whole round in that one session; every open
resets the SoC and repeated opens are what wedged the USB-JTAG unit three times.

1. `python3 exp/plus_smoke.py /tmp/smoke` - one session, nine screenshots
   (Home, My, Settings overview, the four categories, Library, Home) plus the
   mount state and an `[ERR]` digest. Adjust the category-strip taps from the
   screenshots if a tap misses.
2. `python3 exp/plus_formats_check.py /tmp/fmt` - requires `plus-test.{txt,fb2,cbz,pdf}`
   in the card root (already uploaded); opens the first nine grid tiles and
   reports format errors. If the library is still empty, check the log for
   `index build failed` before blaming the formats.
3. WeRead / OPDS / KOReader / BLE entries: open them through My -> 工具 and
   screenshot; they need accounts/servers for a full round, so the goal for this
   pass is "the screen opens and the back path works".
4. The Display-category label split stays the only known visual defect. The
   measurements are in the "Display-category label split" and "Display-category
   forensics" sections above; the one fact still unexplained is that the extra
   two glyphs are painted at +26 px without going through `drawPixel`, i.e. not
   by `renderCharImpl`/`drawGrayscale16Pixel`. Next probe should hook the
   remaining framebuffer writers (bitmap blits and `plotCoverage`) instead of the
   text path.


Formats on hardware (2026-11-11): FB2 and TXT work, CBZ and images do not

Verified with `CMD:open <path>` (persistent reader target, see below):
* TXT: title from the file name, two pages, `Page render ... total=1370ms`, 50%.
* FB2: title/author from the FB2 metadata ("Plus FB2 Test"), chapter heading and
  two wrapped paragraphs, 1/1.
* CBZ (PNG pages and JPEG pages): the reader opens, `[CBR] Failed to render
  page 1/2`. A JPEG opened directly (`ImageViewer`) shows 无效的图片文件.
* `imgtest <path>` (console) reproduces it outside the UI: read=1, write=1,
  conv=0. Three configurations fail the same way (Gray2 scaled, Mono1 scaled,
  Gray2 native) while a 200x150 JPEG succeeds (88,306 B BMP). Larger inputs stop
  after a few KB to a few tens of KB of output.

The strongest lead is not the decoder but the **card dying under write load**:
right after several conversions the card stops answering entirely (INVALID_CRC
at 40/20/10/5/1 MHz, 4-bit and 1-bit, with and without the PSRAM bounce buffer)
and needs a power cycle to come back. The converter reports failure silently
because `ENABLE_SERIAL_LOG` is not defined in that translation unit, so its
`LOG_ERR` lines are compiled out - do not read "no [JPG] lines" as "the
converter never ran".

Mitigations in the tree now: the mount ladder releases the host between
attempts, starts at 20 MHz with internal pull-ups (the card is marginal at
40 MHz even when it does answer), and the missing-card path shows the retryable
storage error page. Next experiments, once the card is power-cycled:
1. run `sdwrite` repeatedly and a `putfile` of ~200 KB, watching for the first
   failure - if plain writes kill it, it is the rail/contact, not the decoder;
2. mount without the PSRAM bounce first (the ladder's second sweep now proves
   the code path) and repeat the same write load;
3. if both die, the card itself is the limit - try another microSD.

`CMD:open` / `CMD:stat` / `CMD:imgtest` / `CMD:retrysd` are bench commands added
for these checks; `open` parks a persistent reader target that expires after
four boots so a crashing book cannot leave the unit in a boot loop.


Hardware driver audit against the vendor BSP and kiikoread rc110 metalio47 (2026-11-11)

Four read-only audits (SD, kiikoread board diff, peripherals, display) were run
against: the CloudZao vendor board support vendored in this port, the vendor SDK
copy, and kiikoread rc110 `boards/metalio47` (the only reference that runs on
this exact S31 board; it lives at `repos/kiikoread-rc110`, and `.scratch/rpr`
is a compatibility symlink to it).

Fixed in this round (build + on-device evidence where noted):

* **SD: rewritten to the reference transport.** DMA now lives in a 4096-byte
  *internal* aligned buffer with `SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF` cleared,
  `check_buffer_alignment` restricted to internal RAM, and
  `unaligned_multi_block_rw_max_chunk_size = 8`. The previous 8 KiB *PSRAM*
  bounce buffer is what killed the card under write load: after a few
  conversions the card stopped answering (INVALID_CRC at every clock/width) and
  needed a power cycle. With the internal buffer, three BMP conversions
  (~250 KB written) no longer take the card down (mount at 40 MHz 4-bit,
  `space:` still answers afterwards).
  The ladder no longer deinits the host between attempts (a failed VFS mount
  already released its own host; the extra deinit stopped the SD clock at an
  arbitrary point), keeps 4-bit first with a 250 ms settle, clears D1-D3 for the
  1-bit profiles, and verifies the mount (CMD13 + one sector read + FAT root
  listing) before publishing the card. `sdmmc_card_print_info` is logged.
* **IO expander order** (`vendor/board/display/epd_board_metalio_eink4_plus.c`):
  output latches are now written *before* the direction registers, and both
  ports' polarity registers are cleared, matching the reference. The TCA9555
  powers up with every output latched high, so the old order drove PWRUP,
  VCOM_CTRL, WAKEUP, motor and PA high for two I2C transfers at every boot.
* **Charger bring-up** (`plus_charger.c`): the CX25601N was never configured -
  its watchdog parks charging after a few hours and nothing re-armed it. A
  read-modify-write of CHG_CTRL1 now clears the watchdog and HIZ and sets
  EN_CHG; verified on device: `PLUS-CHG: CX25601N: watchdog off, charging
  enabled`.
* **Battery percentage** (`HalPowerManager.cpp`) comes from the BQ27220 gauge
  (SOC register) instead of a millivolt curve, which reads ~100 % for most of a
  Li-ion discharge.

Audit findings not yet acted on (ranked, with the file to touch):

1. Display refresh economy: every non-FULL update is pushed as GL16 (48 phases,
   ~600 ms). The DU table exists in `my_waveform.c` and the reference uses DU
   for fast updates; `epd_hl_update_area` is also unused, so `displayWindow`
   always scans a whole frame. Fix in `components/cpyos_ui/HalDisplay.cpp`
   (mode selection + area update), then verify ghosting with the periodic GC16.
2. `CONFIG_LCD_RGB_ISR_IRAM_SAFE` is unset while the LCD feeder tasks run on
   PSRAM stacks reading a PSRAM framebuffer; the reference requires it. Any
   flash/NVS access during a scan can starve the feeder (stripes, discarded
   frame). Add it to `sdkconfig`/`sdkconfig.defaults`; consider the reference's
   64-line LCD bounce (ours is 8 lines).
3. Frontlight is dead by configuration: `FREEINK_CAP_FRONTLIGHT`/`WARMLIGHT` are
   0 for the readpico profile, so `FrontlightManager` compiles to nothing. If it
   is enabled, the duty must be clamped to the vendor's 50 % (511/1023) and the
   warm channel (GPIO7) needs the warmlight capability.
4. PCF8563 writes do not check the CTRL1 STOP bit and do not stop the
   oscillator around a write (reference does both); time can drift silently.
5. Key debounce is 2 frames (~2 e-paper pushes); the reference uses a 25 ms
   stability filter. Short taps on the side keys can be missed.

Reproduce any of these with the bench commands added for this audit:
`CMD:stat`, `CMD:open`, `CMD:imgtest`, `CMD:retrysd`, `CMD:batt`, `CMD:sd`.


SD transport fix verified; image decode still refuses files > 2 KB (2026-11-11)

With the internal-DMA SD driver: mount is 4-bit/40 MHz on the first attempt, the
card print info is logged, and three BMP conversions (~250 KB written) leave the
card mounted (`space:` still answers). That confirms the PSRAM bounce buffer was
the cause of the "card dies under write load" failures.

The image pipeline itself is still broken, now with a precise error because the
book library finally has the shared logging macros (`ENABLE_SERIAL_LOG`,
`LOG_LEVEL=2` added to components/cpyos_books):

    [JPG] JPEG dimensions: 640x480
    [JPG] JPEG decoder scale: 1/1 (640x480 -> 640x480), output=684x513
    [ERR] [JPG] JPEG decode failed (rc=0, err=2)     <- JPEG_DECODE_ERROR

Pattern measured across the uploaded fixtures:
* 200x150 (2.5 KB file) decodes in all three configurations;
* 321x127 (11 KB), 400x600 (114 KB), 640x480 (72 KB), 800x600 (114 KB) all fail
  ~25 ms in, whatever the target size or bit depth.

The split is at JPEGDEC's own 2048-byte file buffer (`JPEG_FILE_BUF_SIZE`): small
files are read once at open, larger ones need the seek/read path. `HalFile::read`
now loops over short reads (same fix as the write path) and `HalFile::seek` is a
plain `lseek(SEEK_SET)`, so the next probe is JPEGDEC's data feeding (compare a
hand-driven read/seek trace with what the decoder asks for), not the card.

`CBZ` uses the same converter, so CBZ pages and the ImageViewer stay broken until
this is fixed; TXT and FB2 are unaffected (they parse through HalFile directly).


Image decode: failures track the *upload*, not the decoder (2026-11-11)

Measured with `CMD:imgtest` plus the new `CMD:hash <path>` (FNV-1a over the whole
file) and a fixture matrix:

| fixture | pixels | file | decode |
|---|---|---|---|
| small.jpg | 200x150 | 2.5 KB | ok |
| noise200.jpg | 200x150 | 25.8 KB | ok |
| solid1200.jpg | 1200x900 | 17.7 KB | ok |
| grid200.jpg | 200x150 | 7.4 KB | **fail (err=2)** |
| noise640.jpg | 640x480 | 168 KB | **fail** |
| j321x127 / j640x480 / j800x600 / p1 | - | - | **fail** |

All are baseline 4:2:0, so subsampling, pixel count and file size are ruled out;
the same dimensions decode with one file and fail with another. The remaining
difference is the bytes on the card: the console `putfile` path reads 64-byte
blocks from stdin and can drop a chunk without changing the final size (the
device stops when it has written exactly `size` bytes, so a lost chunk shifts
everything after it). That produces size-correct, content-corrupt files, which
is exactly the "some files decode, some do not" pattern.

`CMD:hash` exists to settle this: compare its FNV-1a with the host value
(`python3 -c` in the append-only BRINGUP notes). If the hashes differ the upload
path is at fault, not JPEGDEC, and the uploader needs a verify-and-retry loop
(or the device's web `/api/upload`, which is out of reach while the Mac has no
route to the device's Wi-Fi).

Bench note: this unit's USB-Serial/JTAG console wedges after roughly 5-10
open/reset cycles (0 bytes on open, esptool cannot connect, USB still
enumerates). Recovery is a physical power press. Batch any multi-step check into
a single session and close the port between flash and console work.


"Storage card error after a restart" is the card, not the console (2026-11-11)

The user reports the device itself showing 存储卡错误 right after restarting, with
the console silent. Two separate failures compound:

1. **The card stays wedged across resets.** A reset that lands while the FAT has
   dirty write cache (our benchmark image conversions write hundreds of KB in
   bursts) leaves this card answering INVALID_CRC at every clock/width until it
   loses power. A soft reboot does not help; only a real power cycle clears it.
   Mitigations added:
   * `HalFile::close()` now calls `fsync()` before `close()`, so a reset cannot
     abandon a half-committed file/FAT update.
   * At boot the mount is retried for ~30 s (10 x 3 s) before the error page is
     shown, and settings/i18n are reloaded if it succeeds late - a transient
     failure now heals without a power press.
2. **The USB-Serial/JTAG console wedges independently.** Opening the CDC resets
   the SoC by design; doing that dozens of times per session eventually leaves
   the peripheral silent (0 bytes on open, esptool "No serial data received",
   USB still enumerated, no coredump). The device keeps running - the error page
   is visible on screen - so the console silence must not be read as "the
   firmware hung".

Rules for the next sessions, to stop needing power presses:
* Batch every step (upload + hash + imgtest + screenshots) into ONE port open;
  never open the port more than once per verification round.
* Do not open the port while the boot log is streaming, and close it before any
  action that reboots (`open`, silent restart, OTA).
* If an open returns 0 bytes, STOP. Repeated opens do not recover it and risk
  making it worse; report and wait for a power press.
* Long write bursts (image conversion, cache builds) are the card-killer: run
  them only right after a power cycle, and finish the round before resetting.


After flashing, start the app with esptool (2026-11-11)

`scripts/flash_metalio_plus.py` leaves the SoC in the loader; the next console
open resets it, but opens are unreliable right after a flash. Running

    python3 -m esptool --chip esp32s31 --port /dev/cu.usbmodem101 \
        --before default-reset --after hard-reset chip-id

boots the application deterministically (verified: full `shell step` sequence and
`route returned`), so a silent console right after flashing is not a dead unit.

Console sessions: drain the boot log on the calling thread
Opening the port resets the SoC and the tty re-enumerates. Starting a reader
thread while that is in flight can leave the handle permanently ENXIO on writes,
and reopening on transient errors resets the device again (measured: four boots
in one round). `exp/plus_session.py` now drains the boot log synchronously until
`route returned`, then starts the reader thread and only then sends commands.

Development sleep default (user decision, 2026-11-11)
Bench images define `CPYOS_DEV_SLEEP_DEFAULT_NEVER`: the saved
`sleepTimeoutMinutes` is moved to **31 (`SLEEP_TIMEOUT_NEVER_MINUTES`, 永不)**
once, so unattended automation never drops into the standby clock. It is a
one-time migration guarded by `/.crosspoint/devsleep-v2` rather than a permanent
override, so a card that already had a settings file is migrated too and a
timeout picked later in Settings survives the next boot (sleeping still works
and stays debuggable). Release images do not define the macro, so a fresh
install keeps the 10-minute default.

Do **not** write 0 for "never": `fromJson()` clamps it up to `MIN` (1 minute),
which is an immediate standby. `info` prints the effective
`sleepTimeoutMinutes` for a one-line check.

### Console task stack (2026-11-11)

`plus-console` runs whole subsystems inline (`stat`/`hash` walk the FAT layer,
`imgtest` decodes JPEG and writes a BMP). It had a 4 KiB stack and panicked with
"Stack protection fault" on the first `stat`; a panic in the middle of a FAT
write is one of the ways the card ends up needing a power cycle. The stack is
now 12 KiB. If you add a heavier bench command, check
`uxTaskGetStackHighWaterMark` for this task before assuming it is safe.

### `putfile` needs binary-safe console input (2026-11-11)

The USB-Serial/JTAG console VFS defaults to **`ESP_LINE_ENDINGS_CR` on
receive**, which rewrites *every* `0x0D` byte to `0x0A`. Binary uploads through
`putfile` were therefore silently corrupted: the file size stayed exact while
the content changed, so uploaded JPEGs hashed differently on the card (and some
failed to decode). Measured with an 8-byte probe `0D 0A 0D 0D 41 42 0A 0D`:
host FNV-1a `d8ee184e`, card `480a5c9e` — exactly the CR→LF variant.

`cpyos_shell_demo()` now calls `usb_serial_jtag_vfs_set_rx_line_endings(
ESP_LINE_ENDINGS_LF)` (LF = pass-through; the console task accepts both `\n`
and `\r`). After the fix the same probe, `grid200.jpg`, `noise200.jpg`,
`batman.jpg` and a 42 KB CBZ all hash **identically on host and card**.

Do not "fix" this in the host tool: any other uploader (the web UI, a bench
script, a customer's USB session) would still be corrupted. Upload pacing must
also wait for the firmware's per-64-byte `PF_ACK`; time-based pacing still
overran the CDC buffer.

### Boot watchdog and breadcrumbs (2026-11-11)

Boot has been observed to stop between the I2C master coming up and the DRAM
allocation; in that state the unit answers neither the console nor esptool and
used to need a physical power cycle.

* `app_main` arms a 60 s `esp_timer` before `MetalioEpd_Init()`; every path that
  reaches a usable UI calls `plus_boot_done()` (including the storage-error
  page). A stuck boot now restarts itself. Interrupts-off lockups are still
  covered by `CONFIG_ESP_INT_WDT` (800 ms).
* `epd_board_init()` prints WARN breadcrumbs (`boot: probe ioexp`, `boot: ioexp
  ok/absent`, `boot: control gpios low`, `boot: lcd reinit`) because
  `epd_eink4p` logs at WARN by default. The last line printed names the step
  that hung.
* One console open per session: each open resets the SoC, so uploads,
  verification and screenshots belong in a single `exp/plus_session.py` run
  (`u:LOCAL:REMOTE` uploads inline). Chained opens are what wedged the
  USB-Serial/JTAG unit in earlier sessions.

### Frontlight (2026-11-11)

The Plus has a warm/cool LED pair (GPIO6 cool, GPIO7 warm) on LEDC. The shared
`FrontlightManager` maps 100 % to **full** duty, which is twice what the
reference firmware allows, so the port compiles its own
`components/cpyos_ui/compat/HalFrontlight.cpp` instead and keeps the reference
math: `total = brightness * 1023 / 200` (100 % = half scale), then split by
warmth. `FREEINK_CAP_FRONTLIGHT` / `FREEINK_CAP_WARMLIGHT` are forced on for
`CPYOS_PLUS_PORT`, so the brightness and warmth rows appear in Settings and the
reader's light controls work.

Verify without a light meter: `light 100 50` prints `duty_cool=255 duty_warm=255`,
`light 100 0` prints `duty_cool=511 duty_warm=0`, `light 0` prints `0/0`. Those
are the numbers the reference writes; a wrong polarity would show as a lit panel
at `light 0`.

### Dead console after a flash (2026-11-11, mitigations in the build)

Symptom seen three times today: right after `flash_metalio_plus.py` + the
`esptool --after hard-reset` boot, the unit enumerates USB but answers neither
the console nor `esptool chip-id`, and the port node does not re-enumerate for
minutes (so it is neither a reboot loop nor the 60 s boot watchdog firing). A
power cycle is the only recovery found.

Boot logs from the failed boots stop right after `MetalioI2C: new master I2C1
ready`, i.e. at the first call into the expander path, and the deferred
breadcrumb `boot: probe ioexp` never appears.

Mitigations now in the image (each is independent):

1. The **TCA9555 INT handler is registered after the UI is up**
   (`epd_board_metalio_eink4_plus_ioexp_irq_start()`), not during the panel
   bring-up. A retained-low INT from the previous session could otherwise storm
   the CPU before any task runs. The pin is configured during bring-up; a low
   line just waits without an ISR.
2. The boot watchdog calls `esp_restart()` **before** logging, so a stall that
   holds the console lock cannot block the recovery.
3. The shell loop prints `[hb] <s> alive=<n> int_free=… psram=…` every 10 s.
   Future sessions can tell "console works but the app is wedged" (heartbeat
   stops, commands may still fail) from "USB peripheral dead" (no heartbeat at
   all) without guessing.

Operational rule for now: after flashing, run
`esptool --after hard-reset chip-id`, then wait for one heartbeat before the
first console session. Do not chain session opens inside the first seconds of
boot.

### Safe iteration protocol (2026-11-11, after four dead-console episodes)

Flashing over USB while the app runs has a chance of leaving the unit with USB
enumerated but no CPU response; only a physical power cycle (or BOOT-mode entry)
recovers it. Until the cause is proven, use this order:

1. Enter download mode **deliberately**: hold **BOOT** while replugging USB (or
   tapping POWER), release once `/dev/cu.usbmodem*` reappears.
2. Run `scripts/flash_metalio_plus.py ... --slot both`.
3. Boot with `esptool --after hard-reset chip-id`.
4. **Wait for one `[hb]` heartbeat line** before opening the first console
   session; that proves the loop task is alive.
5. If the console stays silent, do not chain more opens: hold BOOT and go to 1.

The SD log mirror (`/.crosspoint/logs/boot.log`, 192 KiB cap, append every 3 s)
now records every `ESP_LOG` line including the heartbeat, so a future dead-USB
episode can be diagnosed by reading the card instead of guessing.

### Wi-Fi needs contiguous internal DMA (2026-11-11, open)

`esp_wifi_init` fails with `ESP_ERR_NO_MEM` on the full UI stack:

* before init: internal free ~93 KB, largest ~57 KB, DMA free ~65 KB;
* it allocates the static pools and dies ("wifi:malloc buffer fail", then
  "Expected to init 16 rx buffer, actual is 8");
* forcing the font reclaim (`CROSSPOINT_PORT_FORCE_FONT_RECLAIM`) frees PSRAM
  only (no internal), so it does not help;
* a 40 KB reserved arena made the baseline worse and did not fix init;
* shrinking the panel line queue from 128 to 96 freed **44 KB** internal
  (free 118 KB / largest 63 KB) and Wi-Fi still failed, while the smaller queue
  produced a `line buffer underrun` on the browser's first 48-phase frame and a
  crash after it. Reverted to 128.

So the radio's requirement is not just "some free memory": the vendor's own
comment says the 128-line buffer exists to absorb WiFi contention, i.e. the two
subsystems compete for the same internal pool by design. Options for a later
pass: run the line feeder from PSRAM with an internal bounce window, shrink the
WiFi pool through a custom sdkconfig of the precompiled lib (not possible
locally), or bring the radio up before the panel allocates and keep it resident
(costs ~100 KB internal forever).

One flash attempt (`write_flash failed`) left the unit absent from USB until a
power cycle; treat a failed write as "device needs a manual reset", not as a
soft error.

### SD log mirror completeness (2026-11-11)

The mirror first missed every shared `LOG_*` line: those go through
`logSerial.print()` in `lib/Logging/Logging.cpp`, not `esp_log`. `Logging.cpp`
now calls an `extern "C"` weak `logMirrorAppend()`, which the port defines to
append into the same ring; `CMD:cat /.crosspoint/logs/boot.log <bytes>` reads it
back. A traced Settings render is ~20 KB, which the serial console drops but the
card keeps — that is how "which draws happened" became answerable.

### Wi-Fi init fixed with a lean runtime config (2026-11-11)

`wifi_init_config_t`'s buffer counts are runtime fields, so the precompiled
driver can be asked for a smaller profile. The port now uses static RX 6,
static TX 8, dynamic RX/TX 8, cache TX 8, RX management 3, management short 6,
AMSDU off. `esp_wifi_init` then succeeds next to the 128-line EPD feeder and the
STA association starts (`connecting to '<saved ssid>'`); the only failure left
during the session was the saved AP being switched off.

### Web transfer verified end to end (2026-11-12)

`web_transfer` was marked "untested on hardware"; it is now exercised on the
unit. The STA path needs an associated station, so start it after a network
consumer has connected (the browser does the silent saved-network connect):

```
browser https://example.com      # connects the saved AP
web                              # prints http://<ip>/ and starts :80
```

Verified over the LAN (host side):

* `GET /` and `/files` (gzip-compressed pages, 200)
* `GET /api/list?path=/` returns the JSON listing
* `POST /api/upload?path=/x` with 17 B / 4 KiB / 128 KiB bodies → `201 Created`
* `GET /api/download?path=/x` returns byte-identical data (sha256 equal)
* `DELETE /api/file?path=/x` → 200 for every fixture
* `GET /api/settings`, `/api/frontlight` (read + set), `/api/screenshot.bmp` (200)
* after the 128 KiB round trip `CMD:sdwrite` still PASSes (card healthy)

Known limits seen on this unit:

* **Calibre wireless works as of 2026-11-13** — the port's own `web_transfer`
  runs an esp_http_server WebSocket listener on :81 (see "Calibre wireless
  upload: three real defects" below). An earlier note here claimed the listener
  could not exist because the upstream server was excluded; that was wrong, the
  listener was compiled but failed to start for three fixable reasons.
* one earlier upload attempt left the SD host wedged: `sdmmc_write_sectors_dma`
  timed out (`0x107`) once, and every later write failed until the SoC was
  reset. There is no runtime re-init of the card yet — treat an SD write error as
  "reboot before trusting writes" until that recovery path exists.

### Saved-network auto-connect: first attempt fails with reason 202 (2026-11-12)

Reported as "the Wi-Fi sheet stops on the list even though an AP is saved".
The device log shows the first association attempt right after the radio starts
being rejected by the AP (`station disconnected reason=202`, AUTH_FAIL) about
0.7 s after `connecting to '<saved ssid>'`; the call then gave up, the sheet sat
on the scan list, and only the scan-based fallback ~40 s later connected.

Fixes in `components/cpyos_ui/compat/WiFi.cpp`:

* `startRadio()` waits for `WIFI_EVENT_STA_START` (up to 2 s) before the first
  `esp_wifi_connect()`;
* `IP_EVENT_STA_LOST_IP` no longer counts as a connect failure unless an
  association was actually established;
* `begin()` retries the association once (250 ms later). Verified on the unit:
  reason 202 on attempt 1, `connecting ... (retry)` 250 ms later, associated in
  2.6 s, connected in 3.6 s, sheet exits by itself and the shelf loads.

The disconnect reason is logged from now on (`station disconnected reason=N`).

### SD write-wedge watchdog (2026-11-12)

A chapter cache reproduced the write wedge (`sdmmc_write_sectors_dma` CMD13
timeout, 0x107); every later write fails until a SoC reset while reads keep
working. Pure SD stress does **not** reproduce it: `CMD:sdstress 20`
(64 KiB write + full-panel scan per round) and 60× `CMD:sdwrite`
(create/write/read/delete) both pass with the radio off, so the trigger is
concurrent Wi-Fi + chapter-cache I/O, not the display.

Mitigation now in the firmware:

* the mount profile is 4-bit **40 MHz** (`mounted 4-bit SD at 40000 kHz`,
  `bus_width=4`);
* `cpyos_sd_remount()` unmounts the VFS and re-runs the mount ladder
  (`CMD:sdremount` → `ok`, then `sdwrite` PASS and a full listing);
* a `sd-health` task writes 8 bytes to `/.sdhealth` every 15 s. Two consecutive
  failures trigger a remount; a failed remount restarts the SoC, so a wedged
  card now recovers by itself instead of waiting for a power cycle. The probe
  file is verified present with the expected pattern, and the normal soak shows
  no false positives.
* `CMD:sdstress <n>` and `CMD:sdremount` are the bench hooks for this area.
* `CMD:sdhealthfail` makes the next probes use an unwritable path, so the whole
  watchdog path can be exercised on demand. Verified:
  `probe write failed (1)` -> `(2)` -> `remounting the card` ->
  `mounted 4-bit SD at 40000 kHz` (50 ms) -> `remounted; open files were
  abandoned`, and `sdwrite` PASS immediately afterwards.
* On remount the port logs one `gpio: conflict found for GPIO[23]` warning (the
  slot re-claims its data pin); the mount and the subsequent I/O are healthy.

Still open: the wedge trigger itself (Wi-Fi + cache). The watchdog converts it
into an automatic recovery; the root cause needs the vendor SD/Wi-Fi coexistence
path or a serialized write schedule.

### Root cause of the "e4plus runs out of memory" saga (2026-11-12)

The Plus kept failing esp_wifi_init with ESP_ERR_NO_MEM, the FATFS write path
with timeouts, and WPA3 association with reason 2, while the same firmware
worked on Read Pico. The board was fine; **the port's sdkconfig did not copy the
kiikoread (metalio47) memory configuration**:

| Option | kiikoread (same S31) | port before | port now |
|---|---|---|---|
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 16 | **16384** (every small malloc pinned to internal RAM) | 16 |
| `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | y | n (lwip/net80211/pp/bt .bss in internal DRAM) | y |
| Wi-Fi init buffers | dyn RX 64 / tx 6 / cache 4 / mgmt 16 | hand-trimmed runtime profile | Kconfig defaults |

Enabling the BSS-segment option moved ~87 KiB of library BSS to PSRAM:
static DIRAM fell from 328,028 B to 240,484 B, heap at boot rose from ~18 KiB to
**130 KiB**, and Wi-Fi now associates (WPA3-SAE H2E, PMF, ch 6, RSSI -36) and
HTTPS GET returns 200. The earlier "lean Wi-Fi profile" was compensating for a
misconfiguration; it is gone.

Two self-inflicted problems found on the way: the `sdstress` bench command had a
static 64 KiB buffer (fixed: PSRAM), and the FAT was corrupted by the earlier
experiments (fixed: `CMD:sdformat`, a one-shot NVS-flagged format at boot).

### Removed the anti-shortage workarounds (2026-11-12)

With the kiikoread PSRAM configuration in place (see above) these are gone:

* `leanWifiConfig()` (runtime buffer trimming) — `WIFI_INIT_CONFIG_DEFAULT()`
  uses the Kconfig values and the buffers land in PSRAM;
* `CROSSPOINT_PORT_FORCE_FONT_RECLAIM` — `NetworkStartup` is back to the shared
  threshold logic (reclaim only below 8 MB PSRAM / 48 KB internal / 32 KB block);
* `wifiMemReserve()` stub and its shell call.

Verified after removal: `plus_network_check.py` PASS (driver init, association,
TLS+GET, browser, JJWXC app) and `plus_soak.py` PASS (11 screenshots,
non-noise ERR: one legacy `[SCT] Deserialization failed` for an older stored
blob).

Also: the local JJWXC session exported on 2026-10-16 is **expired** — a
read-only probe of `/my/fav` with that `sid` answers `302 -> /my/login` from the
host, so the shelf correctly reported "session is dead". A fresh session has to
come from the device login or the browser-session import page.

### Session import, active slot, bench hooks, reader menu default (2026-11-12)

* **A browser `document.cookie` import must store the domain without the leading
  dot.** `CookieJar::deserialize` accepts the seven TAB fields verbatim, and
  `domainMatches()` strips nothing, so rows written with `.jjwxc.net` never
  match `m.jjwxc.net` (the header goes out with no `sid` and `/my/fav` answers
  302 to the login page, which reads as "the session is dead"). Rows with
  `jjwxc.net` + `hostOnly=0` work. Values stay percent-encoded exactly as the
  browser stored them; `JJSESS` is larger than `Cookie::kValueCap` (512) and is
  dropped on purpose — `sid` alone is the session.
* **Check which app slot the unit boots before flashing.** After an OTA the
  bootloader can select `app1` (`0x740000`); a write to `0x10000` then verifies
  fine and changes nothing (the boot log says
  `Loaded app from partition at offset 0x740000`). Flash **both** slots with the
  same image; `Hash of data verified.` twice.
* **Bench `ring`/`confirm` now post to the UI task.** They used to call the
  activity directly on the 12 KB `plus-console` task; an activity confirm that
  measures text rasterizes a glyph through FreeType and overflowed that stack
  (`Guru Meditation … Stack protection fault`, task `plus-console`). The shell
  loop drains the pending action on the same 32 KB task that handles real input.
* **Reader menu default is the CPYOS toolbar on this port**
  (`readerMenuStyle = READER_MENU_TOOLBAR` under `CROSSPOINT_PLUS_PORT`). The
  legacy full-screen list was what shipped first and reads as the wrong panel.
  Verified on glass: centre tap opens the sheet, `目录`/`排版` tabs switch to the
  Contents list (All/Cached/Uncached, fixed update bar, ordinal rows, VIP badges)
  and the Text settings grid. Tab hit rects live at y≈968 (base sheet) and
  y≈476 (expanded panel) in portrait logical coordinates — a tap on the drawn
  label row must use those, not the visual centre of the text.
* **Shelf pages retry once on a mid-body TLS read failure.** Measured twice: a
  44 KB `my/fav` page answered 200 and then died inside the read
  (`verified request read failed`), while the replay succeeded. The page is
  buffered whole before parsing, so replaying cannot duplicate entries.
* **OTA check is disabled on this port** (`OTA is disabled on this port (no
  release channel before hardware validation)`), so the Update page correctly
  reports a failed check; enabling it needs Plus-specific release artifacts.

### JJWXC + font extras on glass, and the hyphenation probe (2026-11-12)

* Imported browser session (seven WAP cookies, dotless `jjwxc.net` domain):
  `WAP session restored: 6 cookies` (JJSESS exceeds the 512-byte value cap and
  is dropped by design), shelf loads **5 pages / 143 books**, the first VIP
  chapter answers `302 -> /buy` and is written as a purchase placeholder with
  `continuing`, and a free chapter caches, prefetches its 8 paragraph-comment
  bodies and packages `JJWXC/女巫晋级指南[西幻]-9695551.epub` (230 chapters).
  The packaged book reopens in the reader (16-level text, page turns) and the
  reader menu's `章评` entry shows `20 / 28` chapter comments with like/reply
  rows.
* Font extras: the manifest is fetched over Wi-Fi (`manifest served by pages`,
  3 packs), `完整 Emoji` installs (825 KB, SHA verified) and the row flips to
  `已安装`. `OTA` check reports "disabled on this port" — expected until there
  are Plus release artifacts.
* `/hyph2.epub` (dc:language=en, two chapters) opens and paginates (2/17), but
  the build prints no `[HYPH] section:` line even though the string is linked,
  so hyphenation is **not verified** on this port; check it via a text-settings
  toggle and visible line-end hyphens if it matters.
* Device regression after the shell/toolbar changes: `plus_soak.py` PASS
  (heartbeat check now tolerates dropped serial lines when the alive counter
  keeps advancing) and `plus_network_check.py` PASS.

### Keystroke latency: transient frames now use DU (2026-11-12)

Every keystroke, overlay step and slider frame took a full 48-phase differential
GL16 scan. Measured on the glass (settings search keyboard, three taps):
render 31 ms + **push 735–739 ms** ≈ 0.77 s per character, which is why typing a
Wi-Fi password felt worse than Read Pico (whose band push is ~440 ms).

`HalDisplay::pushFrameBuffer` now picks the waveform by refresh context:

| context | waveform | measured push |
|---|---|---|
| `TransientUi` (typing, panels, sliders) | **DU, 20 phases** (default) | **397–399 ms** |
| `TransientUi`, every 8th push | GL16 differential (ghost bound) | 735 ms once |
| reader pages / normal frames | GL16 differential (unchanged) | — |

`wave` (console) switches the transient profile at run time for A/B:
`0` = GL16 (old), `1` = DU20 (default), `2` = 14-phase DU (325 ms, ghostiest),
`3` = trimmed 36/37-phase GL (605 ms). Full A/B on the same keyboard:
GL16 739 / GL36 605 / DU20 399 / DU14 326 ms.

DU leaves charge residue on white→black transitions on this panel family (the
reference uses DU for monochrome UI and cleans with GC16), so the shared
periodic FULL cadence is the ghost bound; judge the text field on the glass and
fall back with `wave 0` if it smears. `plus_soak.py` PASS (11 screenshots, no
non-noise ERR) with DU20 active, and `plus_network_check.py` PASS on the new AP.

### Premature action, absolute selection hook, maintenance pages (2026-11-12)

* **"Blank BookDetail" root cause**: a second act* while a job ran was reported as a
  task-creation failure. `startJob` refuses when `jobRunning_`, and `launchJob`
  then ran the OOM path (`job launch failed` + `无法启动任务`), replacing the page.
  Measured: opening a book and hitting 章节列表 1 s later produced exactly that.
  Now `startJob` records `jobStartBusy_` and `launchJob` drops the duplicate with
  `request ignored: <what> is still running`; the page keeps the busy card and
  the next request works. Re-tested: the same sequence logs the ignore line, the
  detail page stays intact, and a later 章节列表 tap loads 56 chapters.
* **`sel <n>` bench hook is real now**: `UiListActivity` gained `debugSelect`
  (clamped, `requestSelection` + redraw), so scripted tests can jump to an
  absolute row instead of counting `ring` steps. Note the System page's row
  order is dynamic (buzzer/clock rows are gated), so index→row mapping must be
  re-read from the screen, not hard-coded.
* **Verified pages**: 屏幕修复 (`[SCREEN] Screen repair: white -> black -> white`
  + three `full clean drive (GC16, whole panel)` + the return-page clean, back to
  Settings), SD 卡固件更新 (opens `FileBrowser` to choose the image), 插件
  (`PluginCatalog` with no plugin directory present), and a free-chapter cache
  (chapter 6: 10.4 KB xhtml, comment bodies prefetched, EPUB + `.key` written).
* Final gates on this build: `plus_soak.py` PASS (11 screenshots, no non-noise
  ERR) and `plus_network_check.py` PASS (TLS+GET, browser, JJWXC session).

### OTA-slot fallback proven on the unit (2026-11-12)

`boothang` (bench) stores an RTC flag and restarts; the next boot consumes the
flag, logs `boothang: stalling before init`, and blocks before display init.
The 60 s boot watchdog then switches OTA slots and restarts. Evidence from the
same power cycle:

```
running=app0@0x00010000 next=app1@0x00740000     # before boothang
... stall ...
running=app1@0x00740000 next=app0@0x00010000     # after the watchdog
```

`part` (bench) prints `esp_ota_get_running_partition()` and the next update
partition, so this is a positive identification rather than "it came back".

Limitation worth knowing: the watchdog only covers a **silent stall**; an image
that panics resets long before the 60 s timer, so a crash loop would keep
bouncing in the same slot. Flash channel for recovery stays esptool over USB.

### Footnotes on glass (2026-11-12)

A crafted EPUB (`<a epub:type="noteref" href="notes.xhtml#fn1">`) renders the
marker underlined, the reader's More panel gains 脚注, and activating it opens
the note. On this probe the inline card path logged
`footnote card: no body for notes.xhtml` and used its designed fallback
(`navigateToHref`), landing on the notes page with the body text. So the
user-facing flow is verified; the card extraction did not produce a body for
this particular zip and should be re-checked with a publisher EPUB before
calling the card path verified.

### EPUB-embedded fonts: glyf works, CFF stays skipped (2026-11-13)

Re-tested A5 on the glass with two crafted packages that share a chapter:

* **TrueType (glyf) embedded face** — used correctly:
  `Embedded EPUB font: SHSansSC @18pt id=-1407635844 (18184 bytes)`, page lays
  out cleanly (screenshot matches the system font's pagination).
* **CFF/OTF embedded face** — skipped by default with
  `embedded EPUB font skipped (CFF scale mismatch): OEBPS/fonts/SHSansSC.otf`;
  the page then renders with the normal reader face and paginates correctly.
  With `embfont 1` the face is accepted and reproduces the known defect on this
  panel: glyph ink ~2x nominal while advances/line height stay at 1x, so lines
  overlap (screenshot captured).

So A5 is now available on the Plus for glyf fonts (the majority of EPUB fonts),
and CFF books degrade to the reader's own face instead of a decorative but
unreadable one. Fixing the CFF metric mismatch itself is still open; the skip
tests the sfnt table directory for `CFF `/`CFF2` (`sfntHasCffOutlines`).

### Fast page turn: DU waveform + a persistence bug (2026-11-13)

* Fast page turns were pushed as **GL16** (890 ms/page). `FAST_REFRESH` with the
  reader's ContinuousReading/TransientUi context now takes the same DU20 path as
  typing: **~400 ms/page**, every 8th frame back to GL16 to bound residue.
* The Text sheet's 快速翻页 switch never survived a reboot on this port:
  `fastPageTurn` and `pageTurnEffect` only exist in `SettingsList` under
  `FREEINK_DEVICE_READPICO`, so the generic JSON serializer never wrote them.
  Both are now written/read explicitly in `CrossPointSettings` (verified:
  `"fastPageTurn":1` in settings.json, still 1 after `reboot`, page pushes DU20).
  Bench: `fastturn 0|1` sets and saves without touching the UI.
* KOReader sync (`KOReaderSettings`) and OPDS (`OpdsServerList`) pages verified
  open; an actual sync/feed needs a server.

### One-command device gate (2026-11-13)

`exp/plus_verify_all.sh [--no-jjwxc] [--book PATH]` runs the three on-device
checks in sequence and fails on the first failure:

* `plus_soak.py` — UI walk (home/library/reader/settings/tools/games), 11
  screenshots, heartbeat and SD assertions, no non-noise `[ERR]`.
* `plus_network_check.py` — driver init, association, TLS+GET, browser, JJWXC.
* `plus_jjwxc_check.py` — shelf (page 1), book detail, chapter list, one free
  chapter cached; fails on `sdmmc_write`/`0x107`/fatal markers.

2026-11-13 run: **ALL PASS** (`shelf 30 books, chapter list 231 chapters`).
Logs default to `/tmp/plus_verify_<HHMMSS>`; `PLUS_VERIFY_OUT` overrides.

### CFF-advance investigation: the number is inside FreeType (2026-11-13)

Instrumented the embedded path (`embedded face metrics/measure` at DBG) and
loaded the same family twice at 20 pt: identical face metrics (`advY=60 asc=49
desc=11`), but the 'A' advance measured **26 px for the TrueType face and 68 px
for the CFF one**. `linearHoriAdvance` and an independent
`FT_Get_Advance(NO_SCALE) * size->metrics.x_scale` both report 68 px, i.e. the
value comes from FreeType's CFF driver, not from TtfEpdFont's fixed-point
conversion. A triage patch in `FtFont` that prefers the units-based advance did
not change the number and was reverted (it would have altered a shared SDK file
on an unproven theory).

Two candidate causes remain, both outside this port's reach without a FreeType
CFF debugging session: the project's own subsetted Source Han OTF carries CFF
charstring widths that disagree with its `hmtx`, or the CFF driver in this
build scales Type2 widths differently. Note the earlier Read Pico A5 check
confirmed *which* font was active, not the line spacing, so this may not be a
Plus-only defect after all.

Policy stays: glyf embedded fonts are used; CFF is skipped with a log unless
`embfont 1` (A/B only).

### Auto page turn and jump-to-percent on the glass (2026-11-13)

More panel → 自动翻页(每分钟页数) opens the rate picker 关/1/3/6/12. Set to 1
and the reader advanced by itself: `Progress saved: spine=128 -> 130 -> 133`,
with the footer reading `自动翻页已启用: 1`; it is suppressed while the toolbar
sheet is open (the first 75 s window produced no turn because the sheet was
still up). Restored to 关.

`跳转到 %` was driven end to end: the dialog opens over the live page
(`EpubReaderPercentSelection`, slider + -/+ + 确认), tapping the slider at the
middle of the track produced **50%**, and 确认 applied it —
`Progress saved: spine=0 offset=4703 page=7` → `spine=0 offset=10775 page=16`;
reopening the book restored **17/17** with the footer at **50%**.
★ Console `k:right` presses did **not** step the dialog value in this pass
(`buttonNavigator.onPressAndContinuous` on the Right button); the touch slider
is the route that works. If a future script needs the step buttons, drive the
-/+ tap zones instead of key events.
★ The BMP capture can lag the push by a frame on 16-level pages: the shot taken
after 确认 still showed the dialog at 50 % while the log and the reopened book
already had the new page. Trust the progress log for gray-page verification.

### Reading orientation on the glass (2026-11-13)

More panel → 阅读方向 → 横屏(顺时针) flips the reader: the section cache is
invalidated (`[SCT] Deserialization failed: Parameters do not match`) and the
page rebuilds as a normal 16-level frame, with no crash and progress kept.
Restored to 竖屏 with the new `orient 0|1|2|3` bench hook (sets + saves) and
confirmed portrait again after reopening the book.

Note for scripted tests: console `tap` takes **portrait-logical** coordinates and
`portraitToPanel` assumes 684x1216, so in landscape the injected point has to be
solved through the renderer transform (center = `tap 342,608`); key events have
no such issue.

### Vertical text on the glass (2026-11-13)

`vtext 1` (bench; sets + saves `verticalText`) then reopening a JJWXC chapter
renders true vertical layout: characters run top-to-bottom and columns advance
right-to-left, with the chapter title and pager rotated onto the column axis and
the progress line on the left edge. No crash, no overlap with the placeholder
artwork. Restored to horizontal text afterwards.

### WeRead online verified end to end (2026-11-12)

With the account logged in on the device (user completed the WeChat scan):

* shelf: 5 books, the first cover fetched over `cdn.weread.qq.com`;
* book detail: title/author/rating 8.7/10/intro, 在线阅读 row already read
  即将支持 (that is the product state, the reader path is 缓存图书);
* 缓存图书 → 指定章节范围 → start 1 / end 3: the app fetches the chapters,
  builds a 3-spine EPUB (`Loaded cache data: 3 spine, 3 TOC entries`) and opens
  it: chapter 1, page 5/17, 12% progress, real text.
* Annotation shading on the glass was confirmed by the user the same day.

### SD diagnostic log: credentials are masked (2026-11-12)

The vendor-style card log already existed (`plus_log_mirror.cpp`, 16 KiB ring →
`/.crosspoint/logs/boot.log`, 3 s flush, 192 KiB rotation, paused while the
radio is busy) but it mirrored raw HTTP debug lines, including `token=` (the
base64 sid) and App-API `loginName`/`loginPassword`. `redactSecrets()` now runs
on every mirrored line — ESP_LOG through the vprintf hook and `LOG_*` through the
weak hook — masking sid / token / loginName / loginPassword / password /
checkdevicecode / authKey / cookie / set-cookie / authorization values.

On-glass proof (`logtest` after deleting the file):

```
I (...) TEST: sid=************ token=*** loginName=***** loginPassword=******* password=****** keep=this
```

The serial console keeps the original line (development only); the card copy is
redacted. The masked line is the one stored in boot.log via `cat`.

### Windowed refresh: present in HalDisplay, unreachable on this board (2026-11-12)

`areatest 5` (bench: draws a 48x48 block and asks `GfxRenderer::displayWindow`
for that rect) produced **five full-page pushes and no `window …` line**. Cause:
the shared renderer still carries the Read Pico quarantine — this port compiles
with `FREEINK_DEVICE_READPICO=1`, and `GfxRenderer::displayWindow` /
`displayPanelWindow` degrade area scans to `displayBuffer(HALF)` for that flag
(the 2026-10-28 soak wedged a unit after ~a dozen forced ticks on this panel
family). `HalDisplay::displayWindow` (blit rect + `epd_hl_update_area`) is
therefore dead code on the Plus today.

Decision: leave it quarantined. Enabling it needs the same bounded-timeout +
recovery design the Read Pico path lacks; the panel family is the same and the
hang history is real. The new `window …` log in `HalDisplay::displayWindow`
stays so a future experiment has immediate timing evidence. Matrix row changed
from U to N/A (quarantined).

### Battery percentage: dead band + critical warning (2026-11-12)

The BQ27220's own SOC is used when valid (4379 mV / 100 % on USB in this
session); the 3300–4300 mV linear curve stays as the fallback. A new pure-logic
filter (`plus_battery_filter.h`, host test `src/plus_battery/run_plus_battery_filter_test.sh`,
14 checks under ASan/UBSan) applies the vendor's idea of a dead band: ±1 point
jitter is ignored, ±2 updates, ≤3 and ≥99 snap immediately. Below 4 % the app
logs `battery critical: N% (… mV); connect power` once per crossing.

Deliberately NOT done: the vendor's CX25601N BATFET register cutoff (the cell
parameters are not documented). The software guard is in place: at <= 3.1 V it
logs one warning, gives a 10 s grace window, then pulses the expander's P1.3
power-off line and restarts. The pulse train still needs a supervised
battery-only session before the "do not run on battery yet" rule can be lifted.

### CPU frequency: why the port stays at 320 MHz (2026-11-12)

The vendor runs 160/240/320 MHz per scene and Read Pico scales 80–240 through
`CONFIG_PM_ENABLE` + `esp_pm` locks. This port has `CONFIG_PM_ENABLE` off and
`HalPowerManager::setPowerSaving()` is a documented no-op.

An attempt to scale manually with Arduino's `setCpuFrequencyMhz()` failed at
link time (`undefined reference to setCpuFrequencyMhz(unsigned long)`) — the
Arduino CPU helper is not linked into this IDF-only image, so there is no
supported one-call switch. The edit was reverted completely (build green again).

Options, in order, if this is picked up later:

1. Enable `CONFIG_PM_ENABLE`, call `esp_pm_configure({max=320,min=160,
   light_sleep_enable=false})`, and wrap the work that must not slow down
   (panel conversion, TLS handshakes, SD bursts, USB CDC) in `esp_pm_lock`s —
   the Read Pico `HalPowerManager::Lock` contract, implemented for real.
2. Or call the SoC RTC clock API directly — **not recommended**: it bypasses
   IDF's peripheral reclocking and can break flash timing/USB.

### DFS on this SoC: configure succeeds, the clock never moves (2026-11-12)

With `CONFIG_PM_ENABLE=y`, `esp_pm_configure({max=320,min=240,light=0})`
returns ESP_OK and `esp_pm_get_configuration` reports 320/240, but the core
stays at **320 MHz** even after a real lock acquire+release bootstrap and 12 s
idle; `esp_pm_dump_locks` lists no holders. So this ESP32-S31 build accepts the
configuration without implementing the frequency transition (likely DFS is not
wired for this new SoC in IDF 6.1), and the vendor's 160/240/320 scene
switching has no supported equivalent here yet.

Everything was reverted: `CONFIG_PM_ENABLE` back to `n`, `setPowerSaving` and
`Lock` are documented no-ops again, the bootstrap toggle is gone. The `cpu`
bench command stays and prints `cpu=320 MHz (PM off)` so a future attempt has a
one-line check. Real battery savings on this board therefore need either a
SoC-side DFS fix from Espressif, the vendor's own RTC clock path (measured,
risky), or the still-unported light sleep/battery work.

### A real Metalio Plus board profile (2026-11-12)

`compat/BoardConfig.h` now carries `METALIO_PLUS`, built as
`plusProfileOverrides(READ_PICO)` so the panel geometry, touch frame and UI
scale stay exactly what the port has been validated with. The overrides are the
four facts that used to be poked into `ACTIVE` at runtime from `shell_demo`:

* `name = "metalio_e4p"` (About page and shared diagnostics);
* `displayController = Epdiy` (the vendor BSP path this port drives);
* `frontlight = {GPIO6, 20 kHz, scale 10, inverted, GPIO7}`;
* `audio.buzzer = PIN_UNASSIGNED` (GPIO2 is the TCA9555 INT line).

Selection is `CPYOS_PLUS_BOARD_PROFILE=1` (defined in both `main` and
`cpyos_ui`); `FREEINK_DEVICE_READPICO` is kept on purpose because the display
policy gates (area-scan quarantine, waveform mapping) key on it.

On glass after the switch: boot to Home, touch navigation, and
`plus_verify_all.sh --no-jjwxc` **ALL PASS** (soak + network). The frontlight
readings taken during that run — `light 80 40` → `245/163`,
`light 100 50` → `255/255` — turned out to be the **evidence of an over-drive**,
not a baseline: see the next section.

### Frontlight: 5× vendor current corrected to the 10 % cap (2026-11-13)

The vendor reference (`repos/metalio-e-ink4-plus/main/boards/metalio-e-ink4-plus/periph/frontlight.cc`;
uses 10-bit PWM with `FL_DUTY_MAX 1023` and `FL_OUTPUT_MAX_PERCENT 10`, i.e.
`duty = 1023 * brightness * 10 / 10000` — **UI 100 % is 10 % of the full scale
(≈102)**. The port's `HalFrontlight::apply()` mapped 100 % to half scale
(`brightness * 1023 / 200` → 511), which is **5× the vendor LED/driver current**
at every setting (the `light 100 50` → `255/255` reading above).

Now `apply()` follows the vendor formula and the vendor channel mix
(`cool = min(100, 2*(100-warmth))`, `warm = min(100, 2*warmth)`), with
`kMaxOutputPercent = 10` next to `kFullScale` in `HalFrontlight.h`. Expected
duties: `light 80 40` → **81/64** (was 245/163), `light 100 50` → **102/102**
(was 255/255), `light 100 0` → **102/0**. `plus_hw_check.py` now asserts 81/64
exactly, so a regression to the half-scale mapping fails the gate instead of
silently over-driving the LEDs again.

The glass will look dimmer at the same UI percentage. That is the hardware
budget the vendor ships with; do not raise `kMaxOutputPercent` without a
current/thermal measurement — the LEDs and the driver are the parts at risk.

### XTC reading verified (2026-11-12)

The repo had an XTC reader but no fixture. `exp/make_test_xtc.py` now writes a
two-page 480x800 1-bit XTC (56-byte header, 16-byte page table, XTG page data):
`wrote /tmp/test.xtc (96132 bytes)`. Uploaded via `putfile` and opened with
`open`: `act: XtcReader`, `Rendered page 2/2 (1-bit)`, page turns keep the
reader; the screenshot shows the generated frame/diagonal letterboxed on the
1216-tall panel. The generator output was re-uploaded (`/test2.xtc`) and behaves
the same, so the fixture is reproducible.

### Hardware-contract check in the gate (2026-11-12)

`exp/plus_hw_check.py` asserts the invariants the port's profile/power/OTA work
promises, in one session: `cpu=320 MHz` (DFS unimplemented), a plausible battery
line (`valid=1`, 3000–4500 mV), `light 80 40` reaching PWM **with the exact
vendor duties 81/64** (the old half-scale mapping produced 245/163 and now
fails the gate), the two app slots at `0x10000`/`0x740000` from `part`, and
`embfont=0` (CFF skip is the default; the check resets it first). `plus_verify_all.sh` now runs it
before soak/network/JJWXC; this build: `PASS cpu/battery/frontlight/slots/embfont`
and the full gate **ALL PASS**.

### Bench lesson: never build a font on the console task (2026-11-12)

A `fontprobe` bench (load `/min.otf` through TtfEpdFont at 20/10 pt and measure
it) panicked the unit: `Guru Meditation … Stack protection fault`, SP below the
12 KB `plus-console` stack bounds, right after the command was accepted. Building
a FreeType face + rasterizing a glyph needs several KB and must run on a task
with a large (PSRAM) stack — the same rule that moved `ring`/`confirm` to the UI
task. The command was removed again (build green, device reflashed); CFF
investigation stays at the documented "skipped by default, glyf works" state.

### CFF advances: not a scale factor; off-task probing crashes (2026-11-12)

Three more data points, all from the real reader path (the only place CFF works):

| size | face metrics (line) | 'A' advance | notes |
|---|---|---|---|
| 20 pt | 60 | 68 | same family as the TTF below |
| 16 pt | 48 | 43 | line scales linearly, the advance does not |
| TTF 20 pt | 60 | 26 | correct |
| TTF 10 pt | 30 | 13 | linear |

So the CFF error is **not** a constant or proportional scaling that could be
divided out: the ratio advance/line changes with size (1.13 at 20 pt, 0.90 at
16 pt). Host FreeType 2.13.2 measures both files at 25 px, so the defect is in
this build's CFF driver/charstring-width path, not in the font or TtfEpdFont's
fixed-point conversion.

Off-task probing is not a way around it: a `fontprobe` worker
(xTaskCreatePinnedToCoreWithCaps: START with 32 KB, then 64 KB PSRAM stack) still
panics with `Stack protection fault` in task `fontprobe` at the same PC
(0x40295d04) while loading the CFF face, although the identical call succeeds on
the main task inside the reader. The command was removed again; `embfont` stays
0 by default and the reader keeps the safe skip.

### Reader-format repeat regression (2026-11-12)

`exp/plus_formats_recheck.py` reopens every format that is on the card through
the persistent `open` target, then a second session asserts the activity and
looks for fatal/parse markers after a page turn: plain EPUB (`/hyph2.epub`),
notes EPUB (`/fn.epub`), XTC (`/test2.xtc`), packaged JJWXC EPUB. All four pass;
single-page fixtures legitimately show no render after a turn, so the assertion
is "parsed and alive", not "a new frame appeared". The check is part of
`plus_verify_all.sh` (`--no-formats` skips it). The formats step crosses four
reboots and takes about 7 minutes on its own, so a full gate is ~12 minutes;
use `--no-formats` for quick iterations and keep the full gate for release checks.

### "Dead console" after a reboot: a stale tty that reads but drops writes (2026-11-12)

Reproduced while chaining test sessions across the persistent `open` reboot:
a session opened `/dev/cu.usbmodem*` during CDC re-enumeration, kept receiving
`[hb]` heartbeats, but **none of its commands ran** (no `open:` echo, device
stayed on Home). That is the same shape as the user-visible "console dead after
flashing" report: the read path works, the write path is dead until the node is
re-opened.

Mitigation (tooling, not firmware): `exp/plus_session.py` now performs a
**console handshake** right after opening — it writes a raw `act` and waits for
`act:` to come back; on silence it closes the node, re-globs the port and retries
(20 × 4 s), and logs `[plus_session] console did not answer handshake` if it
never answers. With that, `plus_formats_recheck.py` (four reopen-and-page cycles,
each crossing a reboot) completes 4/4 and the full gate no longer hangs on the
third item.

Rule for any new script: never assume a freshly opened tty can transmit — prove
it answered once before a plan depends on it.

### Repeat regressions, second pass (2026-11-12)

* **Language switch**: 设置 → 系统 → 语言 → English closes back to Settings with
  the English chrome (`Settings / Display / Reader / Controls / System`) and
  `info` reports `language=0`; selecting 简体中文 returns `language=34`. Only
  EN/ZH are offered (no i18n data partition on this port).
* **Dictionary manager**: 阅读器 → 管理词典 opens `DictionaryDownload`.
* **WeRead cached book**: `/WeRead/亏成首富…-26150754.epub` reopens in
  `EpubReader` and a page turn saves `spine=0 offset=803 page=3`.
* ★ **Dark mode can leak out of the reader**: while probing the reader's More
  panel earlier the 深色模式 row was toggled, and that writes the global
  `screenInverted` setting (Display → 深色模式 showed ON afterwards). It was
  restored to OFF (`inverted=0`). Expected behaviour, but worth remembering when
  scripted taps walk that panel.

### Plain HTTP was silently broken; OPDS verified end to end (2026-11-13)

Symptom: every `http://` fetch (web browser, OPDS/Calibre-style LAN catalogs)
opened the loading card, the peer logged a clean `GET … 200`, and 60 s later the
request failed with `wolfSSL request failed: status -1`. TLS traffic was fine, so
the defect only showed on plain HTTP.

Root cause was in the port's Arduino-Client shim, not in the network stack:
`compat/WiFiClient.cpp` implemented `available()` with a bare
`ioctl(fd, FIONREAD, &pending)`, which on this IDF VFS/lwIP build answers 0 even
with bytes already queued. The SDK's `SecureHttpClient` gates the header read on
`available() > 0` (`readLine`), so it never consumed the response. A new
`httpprobe` bench (drives the compat Client exactly like the SDK does) made it
unambiguous:

```
httpprobe: connect=1 in 11ms
httpprobe: wrote 93 of 93
httpprobe: read 782 bytes in 41ms (loops=9 avail>0=0 connected=8)   ← before
httpprobe: read 782 bytes in 41ms (loops=9 avail>0=7 connected=8)   ← after
```

Fix: try `lwip_ioctl(FIONREAD)` first, then fall back to a
`recv(MSG_PEEK | MSG_DONTWAIT)` probe (returns 1 when a byte is pending, stops
the socket on an orderly shutdown so `connected()` still reports it). TLS paths
use `WiFiClientSecure::available()` (`esp_tls_get_bytes_avail`) and were never
affected, which is why JJWXC/WeRead/font downloads kept working.

Verified after the fix, all on the unit:

* Web browser renders `http://192.168.101.162:8000/opds.xml`:
  `GET attempt 1 status=200 body=589 total=589 in 304 ms`, page painted.
* OPDS end to end against a local OPDS 1.0 feed over HTTP/1.1:
  server list → `Feed parsed as OPDS 1.x Atom` → row "Mini Probe Book / CPYOS"
  → tap → `Downloading: http://…/book.epub -> /CPYOS - Mini Probe Book.epub`
  → `Downloaded 2218 bytes` → `open /CPYOS - Mini Probe Book.epub` renders
  "Chapter 1" in EpubReader (footer 1/1, 100 %). The feed/book fixtures were
  removed afterwards and `/.crosspoint/opds.json` deleted, so the unit is back
  to no OPDS servers.
* `plus_hw_check.py` still passes (it also now turns the lamp off again after the
  duty assertion).

★ Bench note: the shell-demo harness only loads `SETTINGS`/`WIFI_STORE` at
boot, not `OPDS_STORE`. The OPDS tile therefore stays hidden until some OPDS
page touches the store (Settings → 系统 → OPDS 服务器), after which `My → 工具`
shows it. The product build loads every store in `main.cpp`, so this is a
harness-only ordering, not a product bug — but scripted runs must open the OPDS
settings page first.

### Gate robustness after the second full run (2026-11-13)

Script-level fixes so the one-command gate stays deterministic:

* `plus_hw_check.py` asserts the **exact vendor duties** (`light 80 40` → 81/64)
  and turns the lamp off again at the end (it used to leave the bench lamp on).
* `plus_network_check.py` runs `httpprobe example.com 80 /` while the browser
  session still holds the radio and fails unless bytes came back, so the
  plain-HTTP `available()` regression above cannot return unnoticed.
* **Every stage now lands on the tab shell before its fixed-coordinate taps**
  (`open clear` + five or six `back`s + the Home tab). Stages used to inherit
  whatever the previous one left open; a manual JJWXC run once left the unit
  inside that app, and the next soak then tapped the wrong screens
  (`Game2048/SudokuMenu/Calculator did not open`).
* `plus_session.find_port()` now waits up to 25 s for the CDC node instead of
  failing instantly. A stage that starts while the unit re-enumerates used to
  die with a zero-byte log ("session exited 1") and every assertion then failed
  for the wrong reason.
* `plus_jjwxc_check.py` also treats either a free-chapter `chapterContent` GET
  **or** a `/vip/` body GET as the "body stage". The chapter window opens at the
  account's synced position, so on this account the cache run can start inside
  VIP territory and only issue `/vip/` requests (each answered `302 → /buy` and
  stored as a refusal placeholder). The free-chapter decode path is still
  covered by the earlier gate runs and by the packaged-EPUB format stage.

### 33-language UI from the data partition (2026-11-13)

The port had been shipping English + Simplified Chinese only because the pack
was never enabled: `cpyos_ui/CMakeLists.txt` already generated
`build/i18n_pack.dat` (`JILP` v2, 345,504 B, 33 languages) with
`--builtin-langs en,zh_cn`, but `CROSSPOINT_I18N_PACK` was off and the pack was
never written to the card. Enabling it is a two-part change:

1. `CROSSPOINT_I18N_PACK=1` in the component's PUBLIC definitions (the same
   switch Read Pico uses; `I18nPack.cpp`, the emoji partition loader and the
   pack-update prompt compile in).
2. Flash **the pack generated by the same build tree as the firmware** at
   **spiffs 0xCD0000 + 0x100000 = 0xDD0000** (`I18N_PACK_PARTITION_OFFSET` in
   `lib/I18n/I18nPack.h`, base from `partitions_plus_ota.csv`).
   `Hash of data verified.`
   ★ The port's CMake generates `ports/metalio_plus/build/i18n_pack.dat` with
   `--builtin-langs en,zh_cn`, and its key signature/order differs from the
   repo-root `build/i18n_pack.dat` produced by the all-language generator.
   Mixing them boots with `[I18N] language pack built for a different key set`
   and every non-EN/ZH language falls back to English; flash the pack from the
   firmware's own build directory (currently `ports/metalio_plus/build`).

Evidence on the unit: `lang 9` (de) prints
`[I18N] 'DE' loaded from the pack: 9497 B blob + 3636 B offsets` and the UI
switches to German — Home shows "Keine Bücher / Bibliothek / Find books / Me",
Settings shows "Einstellungen / Lesen / Anzeige / Bedienung / System". `lang 34`
returns to Chinese. The `lang` bench now accepts a numeric index
(0..`Language::_COUNT-1`) instead of only `en|zh`, so a packed language can be
selected without walking the settings list.

★ **Long translations exposed two real layout bugs**, both fixed in the shared
sources: the My settings rows drew the label and the right-aligned value on top
of each other ("Texteinstellungen 16 pt" overlapped), and the Home stats strip
drew "This week" over the neighbouring "Streak" column. Both now ellipsize the
label to the space actually left of the value (`MyActivity::drawSettingsCard`,
`InxRecentActivity` stats strip). Chinese/English are unchanged because their
labels already fit.

★ Fallbacks still exist at the *content* level: keys added after the upstream
translation snapshot (e.g. the Home week label, "Check JJWXC") show English in
German because `de.yaml` has no entry — same behaviour as Read Pico. A pack-only
install (app flashed without the data partition) still boots on EN/ZH and
reports the packed languages as unavailable; flash the pack whenever the app
changes the key set.

### SD font families and the CFF containment (2026-11-13)

User fonts on the card were re-checked through the production paths (registry +
settings), not just the font-extras supplement path:

* `/fonts/TestTTF/test-ttf-cn.ttf` (uploaded over the device's own WebDAV,
  `PUT 201` 1,601,524 B in 3.9 s) is discovered (`Found family: TestTTF`),
  loaded resident into PSRAM (`Resident TTF … 1563 KB`), the reader face is
  built with the reading budget (`Reader TTF face loaded: 1024 KB cache / 4096
  glyphs`) and the page renders in that face (`Loaded TTF font: TestTTF @16pt`).
  Setting `sdFontFamilyName` in `settings.json` is what the font picker writes,
  so this exercises the same resolver the UI uses.
* `/fonts/TestOTF/SHSansSC-regular-gb2312.otf` loads the same way but **draws at
  ~2× ink size with nominal metrics** — body text overlaps line after line.
  This is the same defect already seen for embedded CFF faces; it is therefore a
  property of this port's FreeType/FreeInkFont CFF path, not of the EPUB layer.

Because a user can otherwise pick a family that renders unreadably,
`SdCardFontRegistry` now skips CFF files under `CPYOS_PLUS_BOARD_PROFILE` with
`Skipping CFF/OTF file (device renders CFF at 2x): <path>`; the family simply
does not appear in the picker. Read Pico is unaffected (guard is Plus-only) and
TrueType/TTC/`.cpfont` families keep working. The A/B switch for embedded faces
(`embfont 1`) is unchanged.

Test artefacts were removed afterwards (`DELETE` 204s for both families and
`/fonts`), and the settings file was restored byte-for-byte.

### Calibre wireless upload: three real defects, now working (2026-11-13)

The `:81` listener had been logging `httpd_start(81) failed` on every start. Three
separate problems were behind it, all in the port's own `web_transfer` /
`ws_upload`, not in the Calibre protocol:

1. **Control-socket collision.** Both esp_http_server instances used
   `HTTPD_DEFAULT_CONFIG()`, whose control socket takes `server_port + 1`. The
   HTTP file server on 80 therefore owned **81**, so the WebSocket server could
   not bind it. Fixed with explicit `ctrl_port` (`32768` for HTTP, `32769` for
   WS).
2. **Socket budget.** `CONFIG_LWIP_MAX_SOCKETS=10` and both servers defaulted to
   `max_open_sockets=7` (plus a control socket each). The second `httpd_start`
   failed on the socket cap; the HTTP server now uses 5, the WS server 2.
3. **The handler looped inside one invocation.** esp_http_server parses each
   frame's first byte (and thus its opcode) *before* invoking the handler again.
   The old `for (;;)` loop reused a stale `ws_type`, so the second frame's
   boundary shifted by one byte — the device log showed
   `httpd_ws: WS frame is not properly masked` and the client saw a reset after
   the first binary chunk. The handler now processes **exactly one frame per
   invocation**, which is the contract the IDF echo example uses.

End-to-end proof with a standards-compliant client (`websocket-client` 1.9.0):

```
handshake: HTTP/1.1 101 Switching Protocols
server: READY
server: PROGRESS:65536:200000
server: PROGRESS:131072:200000
server: PROGRESS:196608:200000
server: PROGRESS:200000:200000
server: DONE
PLUS-WS: uploaded /sdcard/calibre_probe2.bin (200000 bytes)
```

The file was downloaded over HTTP and matched the generated payload byte-for-byte
(`sha256 95747c03…`). Diagnostics kept: `recv_frame failed: err=N` and
`write failed: errno=N …` (low frequency, only on faults). The 200 KB probe file
was deleted and the server stopped afterwards.

### KOReader progress sync verified both ways (2026-11-13)

The reader's `同步进度` row had only ever been page-checked. A local kosync
server (`GET/PUT /syncs/progress[/<doc>]`, `x-auth-*` headers ignored by the
stub) plus `/.crosspoint/koreader.json` (`username`/`password`/`serverUrl`,
plaintext accepted and re-saved obfuscated) exercised the real client:

* **Fetch**: `Getting progress: http://…/syncs/progress/d8ab2db2…` →
  `Get progress response: 204` (no remote) → the activity offers
  `未找到远端进度 / 上传当前位置？`.
* **Upload**: tapping `上传本地进度` sent
  `{"document":"d8ab2db2…","progress":"/body/DocFragment[2]/body/p[5]/text()[1].234","percentage":0.5625,"device":"CrossPoint","device_id":"crosspoint-reader"}`,
  server answered 200, and the activity returned to the reader by itself.
* **Compare + pull**: with a 20 % record seeded for the same hash the page shows
  `远端 One 20.00% / 本地 Two 56.25%`; `应用远端进度` saved
  `spine=0 offset=387 page=0` and the reader reopened on **Chapter One, 1/17**.

Bench support added while chasing this: `CMD:sync` (deferred hook →
`EpubReaderActivity::debugSync()` → the same `launchKOReaderSync()` the menu
calls) and the shell now loads `KOReaderCredentialStore` at boot — the product
`main.cpp` always did, but the shell harness only loaded settings/Wi-Fi, so the
sync row looked unconfigured on the bench. The test credentials and server were
removed afterwards.

### Final mixed soak on the shipped image (2026-11-13)

Beyond the gate's fixed route, a 30-page reader pass on the protected JJWXC
book, six full panel open/close cycles (Contents/Text/Bookmarks/More), a tab
tour (Home → Library → Home → My → Tools → game → back) and a game launch were
run in one session. Result: **0 real errors** (the only `[ERR]` lines are the
benign `[TTF] Auto hinting unavailable`, which fires whenever a vector face is
built without the autohint module and is the documented fallback), no
`line buffer underrun` / `Frame failed` / panic / canary, and the heap ends where
it started: internal `free 128,567 → 129,639` (min 47,516, largest 63,488
throughout), PSRAM `13.18 → 12.72 MB` (the reader face's resident buffers).

### Reader screenshot was broken by two shared assumptions (2026-11-13)

The reader menu's 截屏 row failed with `[ERR] [SCR] Failed to save screenshot` on
every attempt. Two independent bugs, both in shared code that assumed smaller
panels/cards:

1. **FatFs mkdir has no parent creation.** A book screenshot goes to
   `/screenshots/<title>/…bmp`, and the code created only that last directory.
   On a card that had never taken a screenshot `/screenshots` itself did not
   exist, so the single `mkdir` failed. `saveFramebufferAsBmp()` now creates the
   parent first and logs the directory it could not create.
2. **The BMP row buffer was hard-coded to 68 bytes** (the X3's 528-px row after
   rotation). The Plus row is `684/8 → 88` bytes, so the writer rejected every
   screenshot with `Row size 88 exceeds buffer capacity`. Raised to 152 bytes
   (the 1216-px panel is the tallest supported one).

Verified on the unit: `Screenshot saved to
/screenshots/Hyphen-Probe/Hyphen-Probe_ch1_p2_3pct_28460.bmp` (107,070 B);
downloaded over HTTP and opened — 684×1216 1-bit, upright, page chrome and text
intact. The test file was deleted afterwards.

### QR page view and the JJWXC update row (same session)

* **以二维码显示页面** opens `QrDisplay`; the captured on-glass frame decodes
  with zxing-cpp as a valid QR carrying the page's exact text (674 chars), which
  also re-validates the earlier `QrUtils` version fix on this port.
* **更新章节列表** from the classic reader menu runs the full deep link:
  `Jjwxc` → Wi-Fi → `chapter list fetch: status=0 entries=231` →
  `packaged 231 chapters` / `repackaged chapter list placeholders: 231 chapters`
  → back to `EpubReader` → the reader's own chapter selection opens.
* Bench support: `EpubReaderMenuActivity` now implements
  `debugSelect`/`debugMoveRing`/`debugConfirm` (it keeps its own `nav`, so the
  generic list hooks did not reach it); `sel <row>` + `confirm` drives any menu
  row deterministically.

### Coverage sweep: every remaining page, plus three real defects (2026-11-13)

The feature matrix proved the big surfaces; what was left were pages that
compiled but had never been opened, and capabilities the port had hidden
instead of ported. This round closed both.

**New hooks (bench-only)**: `settings` / `stab <tab>` / `slist` / `saction <n>`
(SettingsActivity opens the Nth *action* row and logs its label),
`my [tab]` / `stats` (MyActivity tabs and the reading-stats hub), plus
`CMD:WIPE <dir>` and `CMD:PAGETURN 0|1`. `UiListActivity` gained a real
`debugConfirm()` — the base class had none, so `confirm` silently did nothing
on every plain list page (Bluetooth, TapZones, About, …). That single omission
is why earlier soak scripts "opened" these pages and stopped there.

**`exp/plus_ui_sweep.py`** (opt-in gate stage `--ui-sweep`) lands Home, opens
Records → the four stats cards, the 17 tool tiles, then every settings action
row, and asserts the expected activity names, no fatal marker and no `[ERR]`.
Segments (`PLUS_SWEEP_PARTS`, `PLUS_SWEEP_TABS`, `PLUS_SWEEP_SKIP`) exist
because `管理字体` deliberately `silentRestart()`s on exit (installing a font
reboots), which kills an all-in-one session.

**Defect 1 — serif font ids were never registered.** The stats suite,
end-of-book screen, library titles and the first-run gesture hint ask
`NOTOSERIF_18_FONT_ID` (840051567). `SystemFontPack` only registers the ids of
the sizes `Typography` loads, so unless the reader size was 18 pt the pages
logged `[GFX] Font 840051567 not found` and drew **no numbers at all** —
visually "empty" stats. `Typography::apply()` now aliases
NOTOSERIF_12/14/16/18 onto body/title/display and includes 18 in the fallback
loop. Verified: hub, per-book stats, heatmap, profile radar and the 58-entry
achievements list all render (`12_stats_*.bmp`, `ach.png`).

**Defect 2 — the water ripple was hidden, not adapted.** Read Pico's ripple is
a staggered-phase band wipe. The vendor BSP already implements exactly that as
`MODE_SPATIAL_WIPE_GL` (ten GL16 bands, each one phase later, `frame_count +=
9`). `HalDisplay::setPageTurnDirection` now arms the next push, both the 1-bit
and the 16-level commit paths consume it, and a vendor patch adds
`MODE_SPATIAL_WIPE_REVERSE` (bit 17) for back turns.
`CROSSPOINT_PORT_NO_WATER_TURN` is gone, so 设置 → 阅读 → 翻页效果 is visible
again. Reader measurement: `gray wipe dir=1`, **1005 ms** vs 875 ms plain GL16
(one extra 10-frame burst, as designed); 11 consecutive `wave WIPE` pushes with
no rejection and the page intact. Note the wipe is a GL16 scan, so a page turn
with the effect on cannot ride the DU fast path — same trade-off as Read Pico.

**Defect 3 — `fastPageTurn` / `pageTurnEffect` were never serialized on this
board** (fixed earlier in `CrossPointSettings`; recorded here because the sweep
is what surfaced it).

**BLE page-turner (complete by reference, no peer available)**: enabling the
toggle starts NimBLE (`BLE HID host started`, `ready: 0 bonded device(s)`) and
reveals 扫描并配对 / 已配对设备 / 映射遥控器按键; the scan view lists real nearby
peers (`yyyy`, `BH1098_BLE_YZ`, `midea`) and `bt scan` reports the full
start/stop memory log. Pairing a physical pager and decoding its reports is the
only untested part, and it needs the hardware.

**Settings coverage (all rows open)**: System 15/15 (`应用`, `隐藏应用`,
`日期与时间`, `WiFi 网络`, `KOReader 同步`, `OPDS 服务器`, `清除阅读缓存`,
`恢复系统设置`, `检查更新`, `SD 卡固件更新`, `屏幕修复`, `插件`, `键盘布局`,
`关于`, `语言`), Controls 2/2 (`点击区域`, `蓝牙`), Reader (`文字设置`,
`管理字体`). `日期与时间` is `ClockSettings`, which embeds timezone/DST —
there is no separate `DateTimeSettings` page on this build. `恢复系统设置` and
`屏幕修复` intentionally stay on the page (popup / full sweep).

### CFF 2× fixed, Wi-Fi lean init confirmed, and a serial-open hazard (2026-11-15)

**CFF/OTF now renders at the right size.** The double scale came from the
vendored FreeType amalgam having no `pshinter`: `glyph_hints` stayed NULL, the
Type 2 interpreter scaled the outline, and `cff_slot_load` scaled it again.
`cffgload.c` now clears `hinting` when no hinter is linked. Host proof:
`test/otf_support/CffSizeHostTest.c` shows hinted == unhinted and CFF/glyf ink
ratio 1.00 (was 2.07). The Plus-only SD/embedded CFF skips are removed; the
device still needs one page render with an `.otf` family (blocked below).

**Wi-Fi driver init works with the lean RX pool.** `wifi` returned 17 networks
(the saved AP at -35 dBm), so `esp_wifi_init` no longer fails with
`ESP_ERR_NO_MEM`; `radiooff` released the radio cleanly. TLS/browser was not
re-tested in this session because the card wedged first.

**Audio vs Wi-Fi DMA contention (new).** When the BH1098 warm-up ran while a
Wi-Fi scan held internal DMA, `i2s_alloc_dma_desc` failed with
`ESP_ERR_NO_MEM` and the audio worker gave up for the whole power cycle
(`PLUS-AUD: audio unavailable after initialization`). A retry with backoff (or
deferring the warm-up while `g_portRadioBusy`) is required; the speaker path
itself was verified earlier when no radio was active.

**Opening the USB-Serial/JTAG console resets the chip.** Every fresh
`serial.Serial(...).open()` produced `rst:0x17 (CHIP_USB_UART_RESET)`, and one
of those resets landed while the SD card was mounted. The next boots then
failed **every** SD profile with `ESP_ERR_INVALID_CRC`, including 1-bit/1 MHz:
the card itself needs a **real power cycle** (PMU off/on), not another reset,
`sdremount`, or reflash. Tooling rule: open the console **once**, wait for the
`act:` handshake, and never loop open/close while the card is in use.
