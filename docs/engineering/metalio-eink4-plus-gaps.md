# Metalio E-INK4 Plus — gaps, vendor differences and pre-flash notes

Status: **UNTESTED ON HARDWARE.** This file records what is deliberately not
implemented, what differs from the stock firmware, and the code-management work
that must follow the first real-hardware session. It is not a support claim.

## 1. Pre-flash blocker found and fixed in software (2026-11-09)

The stock Metalio E-INK4 Plus (CloudZao v2.0.74,
`partitions/v1/16m.csv`) does **not** use `ports/metalio_plus/partitions.csv`:

| Partition | Stock offset / size | CPYOS `partitions.csv` |
|---|---|---|
| `model` | `0x10000`, 400 KiB | — |
| `ota_0` | **`0x80000`, 5 MiB** | `0x10000`, 14.375 MiB |
| `ota_1` | **`0x580000`, 5 MiB** | — |
| `resources` | `0xA80000`, 400 KiB | — |
| `font_data` | `0xAE4000`, 5 MiB | — |
| `spiffs` | — | `0xE70000`, 1.5 MiB |
| `coredump` | `0xFE4000`, 64 KiB | `0xFF0000`, 64 KiB |

Consequences and the fix:

- The old planner wrote the app at `0x10000` and packs at `0xE70000`, which on a
  stock unit is `model` and `font_data`. It would not boot and would overwrite
  vendor data. `flash_metalio_plus.py` now defaults to `--layout vendor`
  (`partitions_vendor.csv`), reads the real table at 0x8000 before writing,
  refuses a mismatch, and only writes `ota_0`/`ota_1` (5 MiB each).
- The full shell is **9,276,080 B** and cannot fit a 5 MiB stock slot. The
  bring-up (3,318,064 B) and reader (2,445,024 B) images fit and are the first
  hardware targets.
- The CPYOS single-slot table is now an explicit destructive
  `--layout port --full-install` (writes the table at 0x8000, requires a full
  backup). It is refused if `otadata` is initialised because that table has no
  `ota_1`; the planner never hand-writes `otadata`.
- Data packs are **not** written in vendor layout. A partition-aware pack layout
  (`model`/`resources`/`font_data`) is still a port task; the bring-up/reader
  images degrade gracefully without them.

Host regression: `python3 -m unittest test.test_flash_metalio_plus` (15 cases)
plus the full `scripts/run_host_tests.sh`.

### 1a. Also fixed in this review (2026-11-09)

- **Area refresh disabled until validated.** `HalDisplay::displayWindow()` used
  `epd_hl_update_area`; the vendor never does this on this panel class, and the
  CPYOS Read Pico port had to gate area scans behind a watchdog. It now degrades
  to a full-frame GL16 push (`displayWindow: area refresh disabled` once).
  `cleanWindow` was already a full GC16.
- **TPS power-on failure now powers the rails down.** The local "do not reboot"
  patch previously returned with `PWRUP|VCOM_CTRL` still asserted. It now calls
  `epd_board_poweroff()` before giving up, so a failed PMIC cannot leave a
  persistent DC bias on the panel.
- **P14 must stay HIGH.** An attempted “turn the unmanaged 4G/BT-audio/PA rail
  off” cut P14 after init; on the first unit the next reset then reported
  `TPS probe failed: ESP_ERR_NOT_FOUND` and no TCA/TPS rails, because the TCA
  output latch survives a CPU reset and P14 also feeds the TCA/TPS path on this
  board. The call was reverted; P14 stays high like the vendor Wi-Fi mode. The
  helper `epd_board_metalio_eink4_plus_aux_rail_set()` remains for a future,
  measured 4G/audio power policy, but is not called at boot.
- **Flash planner hardened** (see §1): vendor/port layouts, real table
  preflight, mandatory full backup with size check, eFuse secure-boot/flash-
  encryption check, table-last write order, post-write app-header check, table
  binary vs CSV validation, erase ranges echoed, package project-name check.
- **Plain-HTTP Client shim fixed (2026-11-13)**: `compat/WiFiClient::available()`
  used a bare `ioctl(FIONREAD)`, which answers 0 on this IDF VFS/lwIP even with
  bytes queued. The SDK's HTTP client gates its header read on `available()`, so
  every `http://` request (browser, OPDS/Calibre LAN catalogs) timed out after
  60 s with `status -1` while the peer logged a clean 200. The shim now falls
  back to a `recv(MSG_PEEK)` probe; TLS paths were unaffected. Verified with the
  new `httpprobe` bench, a browser fetch of a LAN page, and an OPDS feed →
  download → open in the reader (see BRINGUP).
- **Calibre wireless upload fixed (2026-11-13)**: the port's own `web_transfer`
  runs the :81 WebSocket listener; it had never started because the HTTP
  server's control socket took port 81, both httpd instances exceeded
  `CONFIG_LWIP_MAX_SOCKETS`, and the WS handler looped inside one invocation
  (reusing a stale `ws_type` and shifting frame boundaries). With explicit
  `ctrl_port`/`max_open_sockets` and one-frame-per-invocation handling, a
  standards-compliant client uploaded 200,000 B (`READY` → four `PROGRESS` →
  `DONE`) and the file matched byte-for-byte (see BRINGUP).
- **Reader screenshots fixed (2026-11-13)**: two shared-code assumptions broke
  the reader's 截屏 on this panel — FatFs `mkdir` was called only for the last
  path component (`/screenshots/<title>/` on a card without `/screenshots`), and
  the BMP row buffer was hard-coded to 68 bytes (the X3's 528-px row) while the
  Plus needs 88. Both fixed; a real screenshot now lands upright at 684×1216 and
  was downloaded and opened to confirm (see BRINGUP). The same session also
  verified the QR page view (decoded payload == page text) and the JJWXC
  "update chapter list" row (fetch 231 → repackage → back to the reader).
- **Serif font ids now alias the display role (2026-11-13)**: the reading-stats
  suite, end-of-book screen, library titles and the first-run gesture hint ask
  `NOTOSERIF_18_FONT_ID` directly. The system font pack only registers the ids
  of the sizes `Typography` loads (caption/body/title/display + the reader
  size), so unless the reader happened to be 18 pt those pages logged
  `[GFX] Font 840051567 not found` and drew no numbers. `Typography::apply()`
  now binds NOTOSERIF_12/14/16/18 onto the body/title/display roles, and the
  fallback loop includes 18 too. Verified on the unit: the stats hub, per-book
  stats, heatmap, profile radar and the 58-entry achievements list all render
  with zero `[GFX]` errors (`exp/plus_ui_sweep.py`).
- **Page-turn effect implemented via the vendor wipe (2026-11-13)**: Read
  Pico's water ripple (staggered-phase bands) maps directly onto the vendor's
  `MODE_SPATIAL_WIPE_GL` — ten bands, each starting one phase later, i.e. the
  same algorithm. `HalDisplay::setPageTurnDirection` arms the next push, both
  the 1-bit and the 16-level commit paths consume it, and a vendor patch adds
  `MODE_SPATIAL_WIPE_REVERSE` (bit 17) so back turns wipe the other way.
  `CROSSPOINT_PORT_NO_WATER_TURN` is gone, so the setting is visible again.
  Reader measurement: `gray wipe dir=1`, 1005 ms vs 875 ms plain GL16
  (+10 frames, as designed); 11 consecutive wipe pushes with no rejection.
  The animation's look still needs the user's eyes (`CMD:WIPE <dir>`,
  `CMD:PAGETURN 0|1`).
- **Test hooks the coverage sweep needed (2026-11-13)**: `settings`/`stab`/
  `saction`/`slist`/`my`/`stats` console commands plus `debugActivateAction`
  and `debugListActions`; `UiListActivity::debugConfirm()` (the base had none,
  so `confirm` silently did nothing on every plain list page). Those hooks are
  what let `exp/plus_ui_sweep.py` open all 15 System settings pages, both
  Controls pages, the reader pages, 17 tool tiles and the whole reading-stats
  suite without coordinate guessing.
- **BLE page-turner exercised end to end (2026-11-13)**: enabling the toggle
  starts NimBLE (`BLE HID host started`), the page reveals scan/paired/map and
  the live scan lists nearby peers (`yyyy`, `BH1098_BLE_YZ`, `midea`); `bt
  scan` reports start/stop memory. Only the physical pager pairing remains.
- **CFF double-scale root cause found and fixed (2026-11-15)**:
  the vendored FreeType amalgam does not link the `pshinter` module, so
  `cff_slot_init()` leaves `glyph_hints` NULL. The Type 2 interpreter then
  scales the outline itself (`cf2_getScaleAndHintFlag` sees `hinting == true`),
  and `cff_slot_load` scales the same points a second time because
  `builder.hints_funcs` is NULL. Result: ink at `x_scale²` (×2.0–2.7 depending
  on size) while `horiAdvance` is scaled once — exactly the "2× ink, 1×
  metrics" symptom. The earlier 2048/1000 hypothesis was wrong. The fix is a
  guard in `cffgload.c` that clears `hinting` when no hinter is available;
  host test `test/otf_support/CffSizeHostTest.c` now hard-fails if hinted and
  unhinted loads disagree, and CFF vs glyf ink is 1.00. Upstream
  `freeink-sdk` `d9a1304c` ships CFF/psaux/pshinter and the
  `cffgload.c` no-hinter guard, so the former
  `patches/freeink-sdk-cff/cff-otf.patch` is no longer needed (deleted).
  The Plus-only SD/embedded CFF skips are removed.
- **CFF outlines were contained, not fixed (2026-11-13, superseded)**: every
  CFF surface previously drew at ~2× the nominal ink size with nominal line
  metrics — embedded EPUB faces (skipped by default) **and** SD `.otf` families
  (measured: selecting `/fonts/TestOTF/…otf` produced overlapping 2× text).
  `SdCardFontRegistry` and the reader skipped CFF faces Plus-only; Read Pico
  was unaffected because its `FORCE_AUTOHINT` path re-loaded at NO_SCALE.
  Both skips are removed now that the driver scales once.

## 2. Vendor features: implementation status

Fill from the vendor audit (CloudZao v2.0.74) before the first flash. The table
must distinguish **device-damage/blocker**, **core feature missing**, and
**documented gap**. Items already known:

| Vendor capability | Our status | Risk / next step |
|---|---|---|
| Native vibration/haptics | Implemented (TCA P0.3, settings off/weak/medium/strong). Pulse widths now match the vendor constants 35/50/70 ms (2026-11-15) | Vendor drives those pulses at 70 % soft-PWM (50 Hz, 14/18/20 ms on) and has a continuous mode for aging/power tests; CPYOS holds the line steady until polarity/feel is verified on hardware. Continuous mode is not needed by the UI |
| Dual colour-temperature frontlight | **Implemented (2026-11-13)** with the vendor output cap. `HalFrontlight::apply()` uses the vendor mapping `duty = 1023 * brightness * 10 / 10000` (UI 100 % = 10 % of PWM full scale, ≈102/1023) and vendor channel mixing (`cool = min(100, 2*(100-warmth))`, `warm = min(100, 2*warmth)`). The earlier `brightness * 1023 / 200` mapping drove **511/1023** at 100 %, i.e. **5× the vendor LED/driver current**; it is gone rather than kept as a "brighter default" | `plus_hw_check.py` now asserts the exact duties (`light 80 40` → 81/64, old mapping 245/163 fails). Do not raise `kMaxOutputPercent` without the vendor's thermal/current budget; LED and driver ratings are the reason for the 10 % cap |
| 4G + Wi-Fi dual mode | **Ported (2026-11-15), untested without a module.** Official `uart_eth_modem` + `uart_uhci` + `iot_eth` in `components/cpyos_cellular`; UART2 2 Mbaud (TX58/RX59/MRDY60/SRDY3), framed AT/Ethernet, own `esp_netif` + DHCP. `networkType` is the official save+restart switch; the Wi-Fi sheet treats an active modem link as connected; `releaseRadio()` no longer stops it; probe results persist and `cellularforget` re-runs the probe. **Audio no longer refuses while 4G is active**: the BH1098 reset stops the modem first and a `cell-recover` task re-inits it after P14 rises (vendor `Nt26Board` order) | This unit reports no module at the correct 2 Mbaud. Needs a 4G unit for attach/DHCP/TLS/switch-back and the audio+4G re-attach. **P1.4 stays HIGH like the vendor Wi-Fi mode** |
| Wi-Fi station / SoftAP | **Station works on hardware (2026-11-15)**: with the BH1098 I2S DMA released before `esp_wifi_init` (`plus_audio_release_i2s()` from `NetworkStartup::prepare`) and a two-static-RX/RAM-only profile, the driver initializes on the full shell image. **SoftAP is not usable**: the precompiled S31 library allocates a 752-byte beacon event buffer, fails (`alloc eb len=752 type=4 fail`) and faults (`Load access fault`) rather than returning an error; the port refuses the hotspot before touching the driver | A SoftAP path in the S31 lib that returns NO_MEM, or a much smaller image with 16 static RX buffers (kiikoread's AP helper runs on one). Station transfer/WebDAV/Calibre and the USB-drive path are unaffected |
| Software power-off + 3.1 V guard | **Implemented (2026-11-14)**: `epd_board_metalio_eink4_plus_power_off_pulse()` reproduces the vendor sequence with the values from the official `config.h` (TCA P1.3, 15 pulses, 100 ms per level). The shell loop checks the pack every 30 s and, when VBAT ≤ 3100 mV with no adapter, warns and pulses after a 10 s grace, then restarts if the host survives (USB attached). The charger itself stays read-only (no unverified CX25601N writes) | Verify on battery: the pulse must cut the host rail and a power-key press must boot again; on USB the charger may keep the host alive, which is why the guard logs and restarts. **Fast-charge toggle is not ported**: kiikoread writes CX25601N ICHG (500↔1000 mA in 80 mA steps, VREG 4400 mV, IINDPM kept in sync) from `components/metalio_hw/metalio_charger.c`; port it only with a supervised charging session |
| Power/refresh-rate settings | Partial: standby overlay + **light sleep ported 2026-11-15** with the vendor sequence (TPS sleep, bus hold, EPD off, expander mask, P1.7 wake pin, EXT1 GPIO2 ANY_LOW + timer, P5→P14 cut, bus latch) and a wake-reason decision. The P1.7 wake is confirmed by **two low samples 30 ms apart** (kiikoread rule), so a released contact cannot count as a key. No DVFS (CPU fixed at 320 MHz; `esp_pm_configure` is accepted but the clock does not move) | Open HalFile/DIR handles (the reader's Section keeps one for the whole book) are now **parked and reopened** at their saved offset, so standby over a book sleeps as well as over Home. **Touch stays in normal mode during light sleep**; kiikoread puts the FT6336U in deep sleep (`reg 0xA5=3`) and wakes it with a reset pulse + `0xA4=0` (`components/cst836u/metalio_ft6336.c:128`), saving current and preventing phantom INT wakes — port with hardware. Real sleep current/wake timing still needs a battery-only pass. Vendor 160/240/320 has no supported equivalent here yet |
| AI voice (mic/speaker) | **Audio device side verified (2026-11-14)**: BH1098G mode-1 handshake, I2S slave 16 kHz, PA P0.4, speaker + mic (`audio tone`, `audio mic`, RMS 95). **Bluetooth audio page** and **AI voice client** added 2026-11-15: official xiaozhi v1/v2/v3 framing over `esp_websocket_client`, Opus 16 kHz via `78/esp-opus-encoder`, user-supplied URL/token in `/.crosspoint/voice.json`, push-to-talk only (no wake word/AEC/MCP; S31 has no AFE) | Needs a real xiaozhi server and a headset/mic pass; frame parsing is host-tested and vendor-source exact, not live-traffic verified |
| GC2145 DVP camera | **Ported 2026-11-15**: vendor DVP controller + 586-entry GC2145 VGA table + hardware JPEG (YUV420 q80), `/photos/IMG_<epoch>.jpg`, preview via the existing JPEG decoder. LCD_CAM is shared with the panel: `waitForScanIdle` → `bus_hold_low` (epd_lcd_deinit) → capture → `bus_restore` (epd_lcd_reinit) → FULL re-push. P0.5 power, P1.2 shared reset never toggled | Needs hardware: exposure/byte order, first-frame ghosting, two consecutive captures, no-sensor path, DMA stability |
| Official OTA (`burner.cloudzao.com/zh/my-firmware`) | Vendor page/protocol ported (`plus_ota_activity`), but it needs `plus_ota_key.local` (absent) and the vendor server; CPYOS OTA stays hard-disabled. Device-side **SD image** flashing works through the shared `SdFirmwareUpdateActivity`/`flash` path | A CPYOS Plus release channel (manifest + asset) is a release-pipeline task, not a device-code task |
| WeChat mini-program binding/push | Not implemented (and not portable offline: every endpoint/WSS host and the device token come from the vendor cloud) | Needs the cloud service and credentials |
| Optional SD-card logging for user feedback | **Implemented (2026-11-12)**: ESP_LOG `vprintf` hook + the shared `LOG_*` weak hook feed a 16 KiB ring that a dedicated task flushes every 3 s to `/.crosspoint/logs/boot.log`; the file rotates above 192 KiB and flushing pauses while the radio owns the DMA blocks. Credentials (`sid`, `token`, `loginName`, `loginPassword`, `password`, `checkdevicecode`, `authKey`, `cookie`, `set-cookie`, `authorization`) are masked **before** the card write — verified on the unit: `sid=************ token=*** loginName=***** loginPassword=******* password=****** keep=this` | Done; a settings toggle is not needed (always on, bounded). Coredump mirror still open |
| Touch firmware upgrade | Not ported | Vendor upgrades FT6336U FW v03 on boot (47,176 B image). The driver is vendored; if the unit has older FW, touch may still work but this is unverified. Do not flash a vendor FW image without confirming the panel revision |
| VCOM calibration | Not ported: the port never writes TPS65185 VCOM and runs the board default from `config.h`. kiikoread exposes a factory gate (held volume keys at boot) that writes VCOM from a reference measurement | Needs a meter or a known-good optical reference; a wrong VCOM is visible at once but a badly wrong one can stress the panel |
| Panel hardware revision | Vendor `config.h` selects **1.3/1.4** (`XSTL=GPIO4`, `MODE=GPIO19`); 1.2 uses `XSTL=19`, `MODE=36` | Record the silkscreen/revision before judging a garbled or missing image; the wrong revision can make the panel look dead without any software error |
| Factory test / self-test / battery calibration | Partial (`selftest`, `i2c`, `batt`, `touchraw`, `keys`, `slpinfo`) | BQ27220 SOC is preferred and now filtered (`plus_battery_filter.h`: 2-point dead band, snaps at ≤3 / ≥99, host-tested); the voltage fallback stays 3300–4300 mV. A critical pack logs once (`battery critical: … connect power`). **Still open: BATFET cutoff at 3.1 V and the P1.3 power-off pulse** — both are charger-IC writes and need a measured battery session |

### 2a. Battery safety gate for the first sessions

The vendor firmware (a) initialises CX25601N (VREG 4400 mV, ICHG 1000 mA,
ITERM 60 mA, EN_CHG), (b) cuts the BATFET at **VBAT ≤ 3100 mV** with no adapter,
and (c) powers off with a P1.3 pulse train. The port deliberately keeps the
charger **read-only** (no unverified cell parameters) and does **not** cut the
BATFET register; it does implement the P1.3 pulse train and a software 3.1 V
check (30 s poll, 10 s grace, pulse + restart if USB keeps the host alive).
**Therefore: the first battery-only session must be supervised** — confirm the
pulse really cuts the rail, a power-key press boots again, and the pack stays
above 3.1 V; the BATFET write stays open until cell parameters are measured.
The 16 MiB backup is not a battery protector.

## 3. Kiikoread/device-variant differences

The reference tree is `read-pico-reader` rc110. Its E-INK4 Plus target (if any)
and the smaller E-INK4 differ in panel timing/waveform, rails, SD width and
frontlight. Our port uses the **vendor** BSP for the Plus rather than copying
the readpico (ESP32-S3) profile; see
[`metalio-eink4-plus.md`](metalio-eink4-plus.md) §1–§2 for the evidence and the
known non-equivalences (waveform phase count, display acceleration, JPEG,
sleep). Do not reuse the 3.97″ Metalio E-Ink 4 SPI pin table: it is a different
SoC and panel.

### 3a. Port integration gaps (discovered in the 2026-11-09 cross-check)

| Item | State | Next step |
|---|---|---|
| Board profile | **Implemented (2026-11-12)**: the port's `compat/BoardConfig.h` now defines `METALIO_PLUS` (derived from the Read Pico profile: name `metalio_e4p`, Epdiy controller, GPIO6/7 frontlight 20 kHz, no LEDC buzzer on the TCA INT pin) and selects it with `CPYOS_PLUS_BOARD_PROFILE=1`. The runtime mutation in `shell_demo` is gone. `FREEINK_DEVICE_READPICO` stays for the display-policy gates (area-scan quarantine) | Layout/touch/gates verified after the switch (`plus_verify_all` ALL PASS); insets are the same 5/5/8/5 as Read Pico because the panel and portrait UI frame are identical |
| USB MSC port | This esp-tinyusb revision declares only `TINYUSB_PORT_FULL_SPEED_0` on S31 (`HIGH_SPEED_0` is compiled out when `SOC_USB_OTG_PERIPH_NUM <= 1`); the vendor advertises HS UTMI | Keep `FULL_SPEED_0`; verify the negotiated speed/enumeration on hardware before concluding it is broken |
| TLS/BLE internal RAM | Port has `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=n`, Wi-Fi static RX/TX 16/16, NimBLE 4 KiB/internal; the reference uses mbedTLS external PSRAM, Wi-Fi 6/6, BLE external/8 KiB | Match the reference before heavy TLS/BLE use; this is the same internal-RAM failure class as Read Pico. Not changed yet |
| Console | Primary console is UART0 (GPIO58/59), secondary USB-Serial/JTAG; **USB-Serial/JTAG dies when I2C takes GPIO0/1** (observed on the first unit). The bring-up heartbeat reappears after USB re-enumeration, but there is no USB command console | Use a UART0 adapter for interactive commands. Do **not** switch the primary console to USB: it was tried and the app's USB link is stolen by I2C |
| Display orientation | UI is portrait-logical 684x1216; panel/EPDiy is native landscape 1216x684. Stock 2.0.66 maps `(xL,yL)->(yL,683-xL)` before `epd_draw_pixel`. The port wrote logical coordinates directly under `EPD_ROT_LANDSCAPE`, so the UI was clipped and the physical pattern looked rotated (first-unit: eight narrow bars left + wide block right, then black) | **Fixed** in `HalDisplay::pushFrameBuffer`/`blitBwRect`/gray commit. Also reverted the 2.0.74 streaming render/LCD stack to the stock 2.0.66 stack (the 2.0.74 NULL dirty-mask/full-update path did not paint) |
| Render stack version | Port was on vendor 2.0.74 (streaming prefill, `epd_lcd_get_display_lines`, NULL masks); stock 2.0.66 uses the simpler `rounded_display_height` + dirty masks and is known to paint | Reverted `render.c`, `highlevel.c`, `output_lcd/*`, `render_context.*`, `lut.c`, RISC-V stub to 2.0.66; keep the newer board/power refactor and local touch pair/power-off fixes |
| SD ladder | Port has 4-bit 40→1 MHz then 1-bit 5/1; reference also does an idle single-line pass, a USB 20 MHz ceiling and RTC profile memory | Add after hardware validation |
| Power off | Vendor uses a P1.3 pulse train (15 × 100 ms); the port never drives it | Port together with the battery-safety work |
| Panel revision | Vendor `config.h` targets 1.3/1.4 (`XSTL=GPIO4`, `MODE=GPIO19`); 1.2 uses `XSTL=19`, `MODE=36` | Record the unit's revision before judging a dead/garbled panel |
| Bootloader/app version | Stock bootloader is IDF 5.5.4; this app is IDF 6.1 preview. The image format is expected compatible but **not verified** | First boot is the test; if it never paints, restore the backup. A full install that writes our bootloader is a separate, riskier step |

## 4. Branch management (public vs dev)

Current facts:

- The Plus port exists **only as untracked files in `repos/cpyos-public`**
  (`ports/metalio_plus/**`). `git ls-files ports` is empty; a `git clean -fdx`
  or a careless dev→public sync would destroy it. A source-only backup was taken
  on 2026-11-09 at
  `artifacts/metalio_plus_port_20261109.tar.gz` (787 KiB, excludes build/
  managed_components/sdkconfig; SHA-256
  `81d2a4c4ac036449d5076126735e4cdeb88397398d35e1f8ccb98517ad449b79`).
- The small-screen/responsive work is in `repos/crossmux` (`feat/jjwxc`) and is
  not in the public tree.
- Shared files touched by the port are guarded (`CROSSPOINT_PORT_*`,
  `CPYOS_OTA_DISABLED`, `FREEINK_CONTENT_PORT_PSA`) except the I18n include
  spelling and the `WIFI_MODE_NULL`→`WIFI_OFF` alias; neither changes the
  readpico binary. The NBSP/QR/browser fixes are shared pending fixes, not port
  leakage.
- `src/main.cpp` and `platformio.ini` have conflicting edits in the two trees
  (public `OTAPROBE`/standby/release env vs dev debug hooks/small-screen
  partitions). Never copy either file wholesale.

Dev-only work that must survive the eventual sync:

| Area | Files | Conflict point |
|---|---|---|
| Responsive/compact home | `src/components/ResponsiveUi.h`, `HomeDashboardLayout.h`, `InxRecentActivity.{h,cpp}`, `EpubReaderActivity.h` | Public has a fixed 318/140/370 px layout; dev computes it from the panel |
| Small-screen targets | `platformio.ini` (EEGO A4 / Metalio E-Ink 4 partition overrides), `partitions_readpico.csv`, `scripts/nightly_targets.py`, `scripts/git_branch.py` | Public platformio has release-only `readpico_jjwxc`; never copy the whole file |
| Docs | `docs/engineering/small-screen-targets.md`, `eego-a4.md`, `metalio-eink4.md`, `index.md` | Merge entries; do not replace `index.md` |
| Debug hooks | `src/main.cpp` `PUTFILE`, `HEAPINFO`, `FEXT*`, `DETAIL`; `Activity.h` `debugOpenJjwxcImport` | Public has `OTAPROBE`/standby; the private hook must stay out of public |

Never copy either direction: `src/main.cpp`, `platformio.ini`, `README*`,
`LICENSE`, `AGENTS.md`, `site/`, `docs/user-guide/`,
`scripts/build_cpyos_release.py`, `verify_release_artifacts.py`,
`UsbDriveActivity.cpp`. Never commit `src/jjwxc`, `ACCOUNT.txt`, cookies/tokens,
`.scratch`, `artifacts`, `build`, `managed_components`, or device screenshots.

Least-risk plan after the first hardware session:

1. Back up `ports/metalio_plus` (source, scripts, docs, tests; exclude
   `build/`, `managed_components/`, `sdkconfig*`, `dependencies.lock`).
2. Create `work/metalio-plus` in the dev tree; apply the port plus only the
   guarded shared changes. Merge `main.cpp` and `platformio.ini` by hand, keeping
   public release-only entries and dev debug hooks.
3. Build `readpico`, `default` (C3), `readpico_nosource`, `eego_a4`,
   `metalio_eink4` and the four Plus variants. Confirm
   `esp_app_desc.project_name` and that the readpico ELF has **0**
   `PlusTransferActivity`/port symbols.
4. Sync dev→public only at release, with an explicit file list; reapply public
   release files (LICENSE, site, user guide, `readpico_jjwxc`).
5. Never commit `src/jjwxc`, `ACCOUNT.txt`, cookies/tokens, `.scratch`,
   `artifacts`, `build`, `managed_components`, or device screenshots.

Until step 2, **do not run `git clean -fdx` in the public tree**.

## 5. First hardware session order (safe path)

1. Full 16 MiB backup with `flash_metalio_plus.py --layout vendor --backup`.
2. Flash the bring-up image to **both** stock slots
   (`--layout vendor --slot both`), then run `bringup_serial.py`.
3. Check VCOM/rails/panel/touch/SD/frontlight/haptics/battery/RTC/IMU before
   flashing the reader image.
4. Only after the stock-layout bring-up is green, decide between:
   - a smaller shell that fits 5 MiB, or
   - a CPYOS full install (`--layout port --full-install`) with a backup and a
     pristine/selected `ota_0`.
5. Record every deviation here and in `docs/BRINGUP.md`; keep every image
   labelled `0.0.0-untested` until the acceptance matrix is complete.

### Open risk observed 2026-11-15 (not reproduced)

One UI soak run ended in `assert failed: block_locate_free
rom/heap/tlsf/tlsf_control_functions.h:618 (block_size(block) >= *size)`
followed by a reboot. That run also had a fake saved Wi-Fi network and a
FontExtras "missing glyph" prompt opening its Wi-Fi sheet during the library
phase. Two later soaks (26 beats / 11 screenshots each), a direct
`open /stix.epub`, `plus_formats_recheck` and the CFF-subset EPUB all passed
with no assert. Keep this as an unresolved risk: if it recurs, capture the
30 s of `boot.log` before the reboot plus the activity trace; the TLSF assert
means heap metadata corruption, not a deterministic encode/layout error.
