# CPYOS on Metalio E-INK4 Plus (port scaffold + hardware bring-up)

> **Status: hardware bring-up complete on one unit; the per-feature evidence
> table is [`docs/plus-feature-matrix.md`](docs/plus-feature-matrix.md).**
> Boot, display/refresh, input, storage, reader and formats, library/settings,
> apps, JJWXC/WeRead/browser, web transfer/WebDAV/Calibre, audio, frontlight
> and the power card are verified on hardware. Still unverified: 4G (no module
> on this unit), BLE pager pairing, tilt/pinch, battery-only shutdown, USB MSC
> eject and a CFF page render. Do not flash this image to a Read Pico or to any
> other device — it is built for ESP32-S31 only.

> **STOP — the stock Metalio E-INK4 Plus does NOT use `partitions.csv`.**
> The vendor firmware (CloudZao v2.0.74) has two 5 MiB app slots
> (`ota_0@0x80000`, `ota_1@0x580000`), plus `model`/`resources`/`font_data`
> partitions. `partitions.csv` in this directory is the CPYOS **single-slot
> full-install** layout (`app@0x10000`, spiffs@0x9B0000) and is incompatible
> with a stock unit. The normal CPYOS target is the dual-OTA
> `partitions_plus_ota.csv` (`app0/app1 0x660000`, `spiffs@0xCD0000`).
>
> For the first hardware test use **`--layout vendor`** with the bring-up or
> reader image (both fit 5 MiB). The full shell is 6.09 MB: it still does not
> fit the stock 5 MiB slots, but it does fit the 6.375 MiB dual-OTA table. Writing app/data with the old hard-coded
> `0x10000`/`0xE70000` commands would overwrite the vendor `model` and
> `font_data` partitions and would not boot. The planner now reads the real
> partition table at 0x8000 and refuses a mismatch.

This directory is a separate **ESP-IDF 6.1 / ESP32-S31** project. The Read Pico
firmware stays on PlatformIO/pioarduino, whose Arduino core has no `esp32s31`
target. Nothing here changes the Read Pico build or the CPYOS application in
`src/`.

## Known gaps (not claimed as supported)

Everything below is deliberately **not** presented as working. The linked
analysis says why, and what evidence would close each item.

| Gap | Current state | Why / what would close it |
|---|---|---|
| Stock-layout compatibility / shell size | **Bring-up and reader only.** The vendor has two 5 MiB app slots; the full shell is 6.09 MB | The shell fits the CPYOS dual-OTA table (`-B build_shell2`, two 6.375 MiB slots). It does not fit the stock 5 MiB slots; the single-slot table is optional and gives more spiffs space. Never flash the shell at the stock layout; the planner refuses it |
| Vendor-layout data packs | Not written on the stock table | The stock `model`/`resources`/`font_data` partitions are vendor-owned and the CPYOS loaders use offsets relative to the CPYOS spiffs base, which does not exist there. A partition-aware pack layout is a later port task |
| Battery safety / power off | **Do not run on battery without a supervised test.** The charger stays read-only; a software 3.1 V cutoff pulses P1.3 (10 s grace) and the 3 s power card draws a themed "已关机" page before the PMU pulse | The vendor CX25601N UV/BATFET registers and cell parameters are still not written (no documented pack). Battery-only shutdown, cutoff and standby drain need one unplugged session (engineering gaps §2a) |
| Light/deep sleep, PMU/EXT1 wake | **Light sleep verified on hardware** (2026-11-16): vendor park sequence (SD quiesce, TPS/bus, expander mask, P1.7 wake, EXT1 GPIO2 + timer, rail cut); clean USB-less session records `usb=0` then `previous-wake reason=1 stage=14` (power key) and `reason=0 stage=14` (timer). Open SD handles are parked (offset/cursor saved) and reopened, so standby over a book sleeps too. Deep sleep stays unimplemented (the vendor's "off" is the P1.3 pulse, not `esp_deep_sleep`) | Longer battery-only current curve; deep sleep is deliberately not added. The 浅睡 badge is the `sleepStateBadge` setting (default ON) |
| OTA / SD full install | Serial `flash_metalio_plus.py` + device-side SD image flashing + the vendor OTA page. The partner API needs **no secret**: `integrationId`/`firmwareId` are public constants, device deliveries use per-device tokens issued after mini-program pairing, and `/updates/check` takes the running version. `X-Resource-Key`, `X-OTA-Key` and `CLOUDZAO_ACCESS_TOKEN` are retired. See `docs/plus-developer-resources.md` | Live test-channel query works (`0.3.1-plus-test1`); stable returns 404 until published. `.github/workflows/cpyos-plus-release.yml` publishes a `cpyos-plus-*` tag to the CloudZao market; secrets/vars are listed in `docs/plus-developer-resources.md` |
| Audio (speaker) / AI voice | **Speaker, microphone, sound cues, Bluetooth audio, voice memo and the xiaozhi push-to-talk client are ported.** AI voice uses the official headers/hello/framing (v1/v2/v3) over `esp_websocket_client` + Opus, with a user-supplied server URL/token in `/.crosspoint/voice.json`; no wake word/AEC on S31 | See [docs/plus-audio.md](docs/plus-audio.md). Needs a real server + headset; WeChat remains cloud-only |
| 4G + Wi-Fi dual mode | **Ported from the official firmware** (UART2 2 Mbaud framed UART-Ethernet, `iot_eth` + DHCP, `networkType` save+restart). This unit has no module (`present=0`), so only the Wi-Fi path is exercised. Audio and 4G coexist the vendor way: the BH1098 reset stops the modem, and a `cell-recover` task re-inits it after P14 rises | Needs a 4G unit: attach/DHCP/TLS, switching both ways, SIM/CSQ UI, audio+4G re-attach |
| Frontlight | **Ported** (GPIO6/7, 10-bit 20 kHz) with the vendor 10 % PWM cap and a boot current probe; user confirmed light/brightness/warmth | Brightness ceiling is the vendor safe value; no auto-brightness |
| CloudZao developer resources | **Binding + BOOK/BADGE/FONT push implemented** through `X-Resource-Key`; a QR/code guide and `/push-resources/next` probe derive the bound state. The vendor WorkBuddy/account mini-program flows remain separate and are not claimed | Device-test on a real account; `.ebook`/`.ef` remain unsupported |
| SD-card logging for user feedback | **Implemented** (`plus_log_mirror`): `boot.log` mirrors ESP_LOG/LOG_*, rotates above 192 KiB and masks credentials before writing | — |
| Touch firmware upgrade | Not ported | Vendor upgrades FT6336U FW v03 at boot; driver is vendored, upgrade image is not |
| Browser settings writes | `POST /api/settings` is writable (two-pass validation, credential keys refused, live frontlight); `/api/frontlight` applies directly. `/api/opds*` and `/api/wifi*` edit the same stores as the Settings pages (passwords never returned) | Device pass over LAN; Wi-Fi changes take effect on the next connect |
| Device acceptance | **Mostly verified on one unit (2026-11-09…16):** power-on/I2C/SD, display modes, reader formats, library, settings, apps, JJWXC/WeRead/browser, web transfer/WebDAV/Calibre, audio, frontlight, power card, BLE host/scan, clean USB-less light sleep with power+timer wake, charging status dialog | Remaining: 4G (no module), BLE pager (no peer), tilt/pinch (physical motion), battery-only cutoff/VCOM, USB MSC eject, network app flows when the AP is available, BT audio/AI voice (headset/server), physical AI-key press and 浅睡 badge eyes. The camera module is absent on this unit and SoftAP is blocked by the S31 library; see `docs/plus-feature-matrix.md` |

## What builds today

| Build | Contents | Image |
|---|---|---|
| default (`idf.py --preview build`) | Vendor BSP subset: TPS65185/TCA9555 power sequencing, E0470A14-AF epdiy output, FT6336U, 4-bit SDMMC, BQ27220 gauge, GC16 bring-up pattern | **396,800 B** (94 % of the 6.375 MiB port-OTA slot free) |
| `CPYOS_SHELL_DEMO=1` (+ `CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1`) | **The full CPYOS activity shell**: `ActivityManager` + CPY theme + Home/Recent, Library, Settings, Apps, Reader, FileBrowser, image viewer, standby overlay driven by the Time-to-sleep setting, **plus all games, calculator, avatar, buddy, AirPage and PixelSwitch**. The shared web-server activity is replaced by a **native File Transfer / Calibre page** (same HTTP/WebDAV/WebSocket servers, port-81 upload, Calibre, the full plugin runner and the OPDS/Wi-Fi APIs); Wi-Fi/TLS/EPUB/book engine are the real ported implementations. EN+ZH embedded (+33 languages from the data-partition pack); CJK uses the real 14pt/16pt i18n faces with the wide 12pt GB2312 fallback | **6,088,592 B** (9 % free in the 6.375 MiB slot; includes the developer-resource binding UI, font refresh, Wi-Fi TX/memory fix, protocol 1.1 OTA query and release-key wiring) |
| Shell + private JJWXC (auto in a dev checkout) | **Shell + the private JJWXC component**: the same image as above plus `src/jjwxc` (the private `cpyos-jjwxc` checkout). An existing checkout is enabled automatically; `CPYOS_PLUS_JJWXC=0` opts out and `=1` requires it. 32 sources, `CROSSPOINT_BOOKSOURCE_JJWXC=1`, JJWXC app reachable from My → Tools. The source is never in this repository; a forced `=1` fails loudly if the directory is missing. A public-tag build has no checkout and therefore no private component | **6,462,992 B** (221,680 B free in the 6.375 MiB slot) |
| `CPYOS_READER_DEMO=1` | BSP + renderer/fonts/input + **a usable reading demo**: a shelf listing up to 50 `.epub` files from the SD card (tap a row or use prev/next), then ZIP/OPF parse, `Section` layout and `Page` render (text, CSS, images) with the built-in CJK font; tap edges / swipe / cover keys turn pages and cross chapters, `Back` returns to the shelf. No activity shell, settings or network UI | **2,268,256 B** (66 % free) |
| `CPYOS_UI_PROBE=1` | The above + real CPYOS `GfxRenderer`/CPY theme, 12pt CJK font, `I18n` (EN+ZH generated at configure time), `CrossPointSettings` + persistence, `FreeInkUI`, the FT6336U/cover-key input backend, **the complete book engine** (ZIP/miniz, EPUB, TXT, FB2, XTC, CBZ, expat, image codecs, library index) and an on-screen bring-up page that also scans the SD card through `library::buildLibraryIndex()`; the **home cover key runs a real Wi-Fi scan** (esp_wifi station + `esp_tls` for HTTPS clients) | **3,572,992 B** (47 % free) |

Sizes measured 2026-11-15; all four variants build (the `bsp`/`bringup`/`reader`
variants needed `app_update`/`nvs_flash`/`esp_partition` in `main`'s REQUIRES,
fixed the same day). `scripts/build_all.sh` stamps them `0.0.0-untested`;
a normal build keeps `0.3.1-plus-test1`.

Resource baseline for the shell+JJWXC image (`idf.py --preview size`, 2026-11-15): DIRAM **273,658 / 503,744 B (54.3 %), 230,086 B free**; LP SRAM 4,196 / 32,768 B. Internal RAM, not flash, is the scarce resource for networking — re-run `size` after adding subsystems.

| Subsystem | State |
|---|---|
| TCA9555 / TPS65185 sequencing | vendored vendor code, unchanged except "power-on failure returns instead of `esp_restart()`" |
| E0470A14-AF panel output (1216×684, GC16) | vendored epdiy S31 backend; 4-bit framebuffer, full-sheet GC16 pushes |
| FT6336U touch + three cover keys | **ported** (`HalGPIO.cpp`): tap / long-press / swipe classifier (28 px / 60 px / 500 ms), normalized panel coordinates, and **two-finger pinch** read from the chip's second point (cumulative scale, one event per gesture; the reader maps it to one font step). Pinch thresholds/centre are host-tested, hardware-unverified |
| USB presence | **ported**: the USB-Serial/JTAG SOF frame counter (same probe as the reference board) with a 1.5 s hold; drives the charging bolt and `wasUsbStateChanged()`. Charge-only sources are covered by the BQ27220 signed-current fallback and the CX25601N VBUS status (both sampled at 1 Hz); a fully charged battery on a dumb charger can still read as disconnected |
| PMU power key | **ported**: TCA9555 P1.7 → `BTN_POWER` (pressed = low); a tap opens the standby overlay, a 3 s hold opens the PowerOff card (both decided from one hold timer, so the release edge of a long hold cannot open standby on top of the card). Deep sleep is still not implemented (the power rail is PMU-owned) |
| Speaker / microphone (BH1098G) | **ported** (`components/cpyos_metalio_bsp/plus_audio.c`, from the vendor firmware, MIT): UART1 `AT+RX=2` / `AT+MODE=1` handshake, I2S **slave** 16 kHz 32-bit stereo (BCLK42/WS43/DOUT44/DIN45), TCA P0.4 amplifier. Wired to the shared `SoundFeedback` cues through `HalAudioOutput`; serial `audio`, `audio tone [ms]`, `audio mic [frames]`. Verified on hardware 2026-11-14: mode ready, 6,400 frames written, mic RMS 95. The AI voice client still needs the vendor cloud endpoint/token; see [docs/plus-audio.md](docs/plus-audio.md) |
| 4G data path (NT26 modem) | **ported from the official firmware** (`uart_eth_modem` + `78/uart-uhci` + `espressif/iot_eth`, Apache-2.0): UART2 2 Mbaud (TX58/RX59, MRDY60/SRDY3), framed AT/Ethernet, its own `esp_netif` + DHCP. Network type is the official manual setting (`networkType`: WiFi default / 4G), shown only when a module is detected, and applies after a restart. `WiFi.mode/status/getMode/disconnect/localIP/SSID/RSSI` map onto the modem. **Compiled in, untested without a 4G unit** (this unit probes absent at the correct 2 Mbaud). See [docs/plus-cellular.md](docs/plus-cellular.md) || microSD (4-bit SDMMC, FAT + UTF-8 LFN) | mounted at `/sdcard` with a speed ladder (4-bit 40/20/10/5/1 MHz, then 1-bit 5/1 MHz) so marginal cards still mount; CPYOS `HalStorage` runs on POSIX VFS over it |
| BQ27220 fuel gauge | **ported** (`plus_battery.c`): mV/mA/SOC on the shared I2C1 bus; voltage→percent estimate; charging = current > 20 mA |
| CX25601N charger | **status read-only** (`plus_charger.c`): `STATUS1` (0x1E) gives VBUS type and charge phase; used as the third USB-presence source and printed by `batt`. Charge parameters stay at vendor defaults — the cell's charge voltage is not documented here, so reconfiguring VREG would be a guess |
| CPYOS renderer + CJK text | real `GfxRenderer`/`EpdFont`, 1-bit buffer converted to epdiy 4-bit |
| CPYOS i18n + settings + persistence | **ported**: `I18n` keeps EN+ZH in the image and loads the other **33 languages** from `build/i18n_pack.dat` in the spiffs partition (PSRAM tables); the bundled Noto Emoji face is loaded from `build/emoji.dat` in the same partition (no pack = emoji fall back to tofu, no crash); **offline pinyin input/search and 11-language hyphenation** load `build/pinyin.dat` / `build/hyphen.dat` from the same partition (SD copies take precedence); `CrossPointSettings`, `PersistableStore`/obfuscation, `KOReaderCredentialStore`, ArduinoJson |
| CPY theme engine (`UITheme`/`CpyTheme`/`BaseTheme`) | **ported and linked**; the probe draws its card frame and themed status bar |
| FreeType + `TtfEpdFont` + `SdCardFontSystem` | **compile and link** (vector fonts, font packages loader) |
| Book engine | **ported and linked**: ZIP/miniz, EPUB (OPF/nav/XHTML/CSS/hyphenation), TXT, FB2, XTC, CBZ, expat, JPEG/PNG/GIF decoders, `library::buildLibraryIndex()` |
| CPYOS reader path | **demo ported** (`main/reader_demo.cpp`): discovery → EPUB → `Section` → `Page` → display, with tap/swipe/cover-key page turns and chapter crossing |
| JJWXC (private component) | **auto-enabled for local dev when `src/jjwxc` exists** (`CPYOS_PLUS_JJWXC=0` opts out, `=1` requires it): all three CMake files (`cpyos_app`, `cpyos_ui`, `main`) add `app/*.cpp` + `protocol/src/*.cpp`, include dirs, `CROSSPOINT_BOOKSOURCE_JJWXC=1` and the `privateSource()` link anchor. Missing source under a forced `=1`, or zero sources, is a fatal configure error. Verified build: 32 sources, 440 `JjwxcActivity` symbols, image 6,462,992 B. Never commit the private source; it is gitignored. Public-tag builds intentionally have no checkout and stay closed-source; the AppCatalog hides the tile instead of leaving a dead tap. Hardware paths untested |
| Refresh modes | **honoured**: `FULL_REFRESH` keeps the GC16 conditioning pass, `HALF_REFRESH`/`FAST_REFRESH` use epdiy's differential GL16 (CPYOS's Read Pico promotes FAST to HALF for the same ghosting reason). Periodic full refreshes come from the shared policy, so ghosting is bounded by that cadence. `panel` vs `panel fast` is the on-device A/B; ghosting/legibility unverified |
| CPYOS Activity shell | **ported** (`CPYOS_SHELL_DEMO`): ActivityManager + Home/Library/Settings/Apps/Reader/FileBrowser **and the full app set** (games, calculator, avatar, buddy, AirPage, PixelSwitch). The shared web-server/Calibre pages are replaced by `PlusTransferActivity` under `CROSSPOINT_PORT_NO_NETWORK_SHELL`: it starts `web_transfer` + `ws_upload`, shows the address and live upload progress, offers a two-tap **USB Drive** action (hands the card to a host; power-cycle to return) and releases the radio on exit |
| CPYOS protected EPUB (`<book>.key`) | **ported**: the shared glue is built with a PSA backend (AES-128-CBC + SHA-1/SHA-256; the legacy RSA/PKCS#12 client helpers are refused). Boot logs `CPRO: PSA self-test ok (AES-128-CBC + SHA-256)` (or `failed`). Not verified on hardware |
| SD file transfer (HTTP) | **ported** (`main/web_transfer.cpp`, IDF `esp_http_server`): one page with list / download / upload / delete under the SD root, plus the documented API subset — `GET /api/status` (documented fields plus live battery/charger and the `?plugin=` deviceId derived from the NVS device secret), `GET /api/files`, `GET /api/screenshot.bmp` (top-down 1-bit BMP of the live framebuffer for remote bring-up), `GET /api/frontlight?brightness=&warmth=&on=`, read-only `GET /api/settings` and `GET /api/diag`, `GET /download`, `POST /upload` (multipart, `<file>.part` then rename, overwrites like upstream), `POST /mkdir`, `POST /rename`, `POST /move` (files only) and `POST /delete` (single `path` or JSON `paths`, empty folders only). Paths are decoded and resolved under `/sdcard`, `..` and `/.crosspoint` are rejected. Host regressions: `bash test/run_transfer_paths_test.sh` (ASan+UBSan; decoding, traversal, private-dir, root mapping) and `bash test/run_multipart_test.sh` (streaming parser, boundary splits, truncation). **No authentication** — start it on a joined Wi-Fi network (`web`; join with `wificonnect <ssid> <pass>`). The SoftAP path is refused on this image (see the Wi-Fi row) |
| WebDAV (file managers) | **ported subset** (`main/webdav.cpp`, wildcard `/*` matching after the exact routes): OPTIONS, PROPFIND (Depth 0/1), GET/HEAD, PUT, DELETE, MKCOL, MOVE/COPY (files only), LOCK/UNLOCK (dummy token) and PROPPATCH (no-op). Same path guards as the HTTP API (no `..`, no `/.crosspoint`); writes go to a temp file then rename. Mount `http://<ip>/` from a WebDAV client; formatting helpers have a host test (`bash test/run_webdav_format_test.sh`). Untested with real clients |
| Calibre wireless upload (WebSocket) | **protocol ported** (`main/ws_upload.cpp`, port 81): the documented `START:<name>:<size>:<path>` → `READY` → binary chunks → `PROGRESS`/`DONE` contract, one upload at a time, `.part` then rename, documented `ERROR:` wording. The Calibre **activity** is still compiled out, so start the server with `web ...` and point the plugin at the device. Parser has a host test (`bash test/run_ws_protocol_test.sh`, ASan+UBSan); untested with the real plugin |
| Font-extra packages | **compiled with the public manifest URLs** (`HttpDownloader` + `esp_tls`); install/verify/fallback code is real but has not been exercised on the device |
| 16-level text | **ported for pages**: a 4bpp PSRAM canvas is converted to the epdiy 4-bit framebuffer and pushed as GC16 |
| Partial refresh / windowed push | **Area refresh enabled with a 3 s scan watchdog.** `HalDisplay::displayWindow` pushes only the requested rows; `cleanWindow` force-drives the dirty rectangle with `epd_hl_update_area_full` and falls back to a whole-sheet GC16 only if that call fails. `displayBuffer`/`cleanWindow` are both watchdog-guarded (no reset key on this board). `CMD:cleanrect x y w h` returns ok=1 for test rects; the on-glass "only that area flashed" check is still pending |
| Wi-Fi | **station ported** (`esp_wifi`): connect/status/scan/RSSI/localIP; credentials live in `WifiCredentialStore` on SD and Settings → Wi-Fi opens the real `WifiSelectionActivity`. The AP/APSTA code exists but **SoftAP faults on this image**: the precompiled S31 library fails to allocate its 752-byte beacon event buffer (`alloc eb len=752 type=4 fail`) and then takes a Load access fault, so the hotspot is refused before the driver is touched. Two static RX buffers plus a RAM-only driver profile keep station association and TLS working (2026-11-15). |
| TLS clients | `WiFiClientSecure` uses IDF `esp_tls` (certificate bundle; `setInsecure()` supported). HTTPS activities that use wolfSSL directly still need a SecureNet replacement. |
| USB mass storage | **ported** (`main/usb_msc.cpp`, `esp_tinyusb` 2.4): one LUN over the microSD card. `usbmsc on` detaches the app FAT/VFS without deinitializing the SDMMC host, pulls **ANA_SW (GPIO36) low** to route the S31 HS UTMI to TinyUSB (port 0 is the HS port on this single-OTG-port SoC), exposes the raw card, and freezes the activity loop; there is no remount path — power-cycle to restore. The USB-Serial/JTAG console may disappear while OTG owns the PHY. **Verified on hardware** (the Mac mounted the card and wrote a 7 MB file); one eject path left USB dead, so power-cycle is the recovery. A no-reboot reclaim path is documented as a remaining task |
| BLE HID host | **ported and exercised on hardware** (`components/cpyos_ui/ble_keyboard_host.cpp`, IDF NimBLE) and wired into the shared CPYOS input stack: **Settings → Controls → Bluetooth** (shared `BluetoothSettingsActivity`) starts NimBLE, scans, connects (Just Works bonding), lists paired peers (forget supported) and disconnects; once enabled in settings the shell keeps the host alive in the reader automatically (the reference main-loop lifecycle, with the reader memory gate and a 2 s retry); keyboard boot reports + consumer-control (scan prev/next) are decoded into `KeyEvent` and routed through `BleInput`/`MappedInputManager` page-turn mapping. Reports with a Report ID prefix are handled; newly bonded peers persist in NVS (four slots, oldest evicted, addr/name in separate keys); keyboard held keys auto-repeat after 500 ms while consumer page keys are debounced to one page per 500 ms. A fresh map defaults Left/Right/PageUp/PageDown to page turns, the scan is active, and the NimBLE host allocates from PSRAM (`bt scan` listed 8 peers at 34.7 KB internal free). The last peer is reconnected after a drop (4 s, five attempts). Unknown HID usages and one-byte vendor codes reach the capture page; passkey entry is not produced. `bt on|off|scan` console diagnostics. The pure report-decoding helpers live in `components/cpyos_ui/ble_hid_decode.h` and have a host regression: `bash test/run_ble_hid_decode_test.sh` (ASan+UBSan; ASCII/shift, specials, press/held/replaced/rollover edges, 8- and 2-byte reports, 9-/3-byte Report ID stripping, consumer usages). Scan verified on the unit (`bt scan 5` listed 8 peers; the Bluetooth page shows scan/paired/map rows once enabled). Pairing a physical page-turner is the remaining untested part |
| OTA | **Vendor plus_ota enabled** (CloudZao protocol 1.1): `X-Resource-Key` + STA `Device-Id` at `/api/firmware-ota/check`, test channel via `X-OTA-Channel: test`; the client compares versions and writes the inactive OTA slot. The shared CrossMux `OtaUpdater` remains `CPYOS_OTA_DISABLED` because its manifests target Read Pico. Live test-channel query works; stable stays 404 until a stable version is published |
| IMU (SC7A20H) | **ported**: the shared `Imu` driver runs on a real Arduino-`Wire` backend over the BSP I2C1 bus; the shell starts the tilt sensor and feeds motion page-turn (tilt/shake) each loop. Unverified on hardware |
| Frontlight (cool/warm) | **Ported and confirmed by the user** (GPIO6/7, 20 kHz, vendor 10 % PWM cap): brightness/warmth controls work, `light 80 40` maps to the vendor 81/64 duties. `CMD:light` and `/api/frontlight` update the hardware live; the earlier "stub" note was stale |
| Haptics (motor) | **ported**: the shell drives the TCA9555 P0.3 motor through the BSP's shadowed `..._motor_set()`. A new key/touch press starts one non-blocking pulse (35/50/70 ms for the weak/medium/strong settings, ≥100 ms apart, matching the vendor constants); "Off" stops it. The vendor drives pulses at 70 % soft-PWM; this port holds the line steady until polarity/feel is verified on hardware |
| Side volume keys | **ported**: TCA9555 P1.0/P1.1 (active low, vendor decode) map to Up/Down on every screen, alongside the cover keys and touch. Polarity/order unverified on hardware |
| Water-ripple page turn | **ported as the vendor spatial wipe**: `HalDisplay::setPageTurnDirection` arms the next commit with `MODE_SPATIAL_WIPE_GL` (ten GL16 bands, each one phase later — the same staggered-phase idea as Read Pico's `e0470_page_turn`), on both the 1-bit and the 16-level push paths. `MODE_SPATIAL_WIPE_REVERSE` (bit 17, vendor patch) carries the back-page direction. Measured in the reader: `gray wipe dir=1`, 1005 ms vs 875 ms plain GL16 (+10 frames). The setting is visible again; visual quality still needs the user's eyes |
| PCM playback (I²S), AI key | not ported. RTC (PCF8563), **SNTP time sync** (`esp_netif_sntp`, one server because this build uses `CONFIG_LWIP_SNTP_MAX_SERVERS=1`, China/mainland server selected by the content profile) and the fuel gauge (BQ27220) **are** ported |

**Hardware status**: the bring-up is verified on a real unit; the authoritative,
per-feature evidence table is
[`docs/plus-feature-matrix.md`](docs/plus-feature-matrix.md). Rows above marked
"unverified" are the ones that still need a device pass.

## Why this is a separate target

- Metalio E-INK4 Plus uses **ESP32-S31** (RISC-V, 320 MHz), **16 MB DIO-80 flash**,
  **16 MB Octal PSRAM @ 200 MHz**, a 4.7″ 1216×684 parallel panel (E0470A14-AF),
  TPS65185 PMIC, TCA9555 IO expander, FT6336U touch, 4-bit SDMMC and USB HS.
- The existing `metalio_eink4` PlatformIO environment is the **3.97″ Metalio
  E-Ink 4** (ESP32-S3 + SSD1677 SPI). It is not the Plus.
- `esp32s31` is still a preview target: every command needs `--preview`, and the
  build requires `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`.

See [`docs/engineering/metalio-eink4-plus.md`](../../docs/engineering/metalio-eink4-plus.md)
for the hardware matrix, the rc92→rc110 code differences, license provenance and
vendor cautions.

The BSP reference is the vendor MIT project at commit
`2b2c36ae58f32a9cb1a721d1000883a2693651a4` (v2.0.74), plus board notes from the
reference firmware's `boards/metalio47/`. The needed subset is vendored under
`components/cpyos_metalio_bsp/vendor/board/`; see its `PROVENANCE.md`.
No external checkout is required to build.

## Build

```bash
. /path/to/esp-idf-6.1/export.sh

cd ports/metalio_plus
idf.py --preview set-target esp32s31      # first time only
idf.py --preview build                    # BSP bring-up image

CPYOS_UI_PROBE=1 idf.py --preview reconfigure build   # renderer + input page
idf.py --preview reconfigure build                     # back to BSP-only

# shell; a local src/jjwxc checkout is auto-enabled (source must never be committed):
git clone git@github.com:Yana-Hangabina/cpyos-jjwxc.git src/jjwxc   # local dev only
CPYOS_SHELL_DEMO=1 CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1 \
  idf.py --preview reconfigure build
# CPYOS_PLUS_JJWXC=0 opts out; =1 forces the component and fails if missing.
rm -rf src/jjwxc                                        # before any public commit

# or build all four and copy UNTESTED-labelled images + hashes out:
bash scripts/build_all.sh [output-dir]
```

### PDF reader (opt-in)

The shell can carry the shared PDF reader while the MuPDF engine stays out of
app0.  Build with `CPYOS_PDF=1` (configure-time):

```bash
CPYOS_PDF=1 CPYOS_SHELL_DEMO=1 CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1 \
  idf.py -B build_shell --preview reconfigure build
```

Two artifacts must be staged on the SD card; `scripts/stage_pdf_sd.py` copies
them and verifies the digests:

- `build/pdfmod-rv.bin` - the RISC-V engine, linked against this exact firmware
  ELF (`scripts/build_pdf_module.py`; the recipe is in AGENTS.md section 67.14).
- `build/pdf_full_assets.bin` - the font pack + 71 CMaps in one PFPK bundle
  (`tools/gen_pdf_assets.py`).  The first PDF open expands it to
  `/.crosspoint/font_pack.dat` + `/.crosspoint/cmaps/`.

```bash
python3 ports/metalio_plus/scripts/stage_pdf_sd.py --card /Volumes/SD \
  --engine build/pdfmod-rv.bin --bundle build/pdf_full_assets.bin
```

The engine is paired with the running image through the ELF SHA at image
offset `0xb0` (esptool writes it; do not use a version string).  Rebuild the
engine whenever the app is rebuilt, then re-stage it.

Serial checks: `pdfassets` (expand the bundle), `pdfengineurl <https-url>` (point
the updater at a test server; empty clears it), `pdfengineupdate` (manifest ->
download -> install, prints the state and install status).  The production
manifest URL is pinned per release to
`https://github.com/Yana-Hangabina/cpyos/releases/download/cpyos-plus-<ver>/plus-pdf-engine-manifest.txt`
(a test build uses its own tag).  The release workflow builds and uploads
`plus-pdfmod.bin`, `plus-pdf-assets.bin` and that manifest next to the firmware;
copy the first two onto the card with `stage_pdf_sd.py` (rename the assets file
to `pdf_assets.bin` when copying by hand).

- The probe/reader/shell is selected at **configure** time, so toggling
  `CPYOS_UI_PROBE`/`CPYOS_READER_DEMO`/`CPYOS_SHELL_DEMO` needs `reconfigure`;
  a plain `build` keeps the previous choice.
- `scripts/build_all.sh` refuses to emit an image whose `esp_app_desc.version`
  is not the `0.0.0-untested` label.
- If `export.sh` looks for a different virtualenv than `install.sh` created, set
  `IDF_PYTHON_ENV_PATH` explicitly (for example
  `~/.espressif/python_env/idf6.1_py3.12_env`).
- **CMake 4.x + ESP-IDF 6.1**: the first fresh configure of a build directory
  fails with `build_properties.temp.cmake: Syntax Warning ... IDF_VER="v6.1""`.
  Run `python3 scripts/patch_idf_cmake_quotes.py $IDF_PATH` once (idempotent)
  to escape quotes in IDF's property writer.
- The first configure downloads two managed components (`espressif/esp_tinyusb`
  and its TinyUSB dependency) from the ESP Component Registry; the port then
  builds offline. `managed_components/` is gitignored.
- **Changing `sdkconfig.defaults` does not update an existing `sdkconfig`.**
  Delete `sdkconfig` and re-run `idf.py --preview set-target esp32s31` when the
  defaults change (this also wipes `build/`).

### Local build storage (do not "clean up" these while porting)

This port cannot share a toolchain with the PlatformIO targets, so it owns three
directories that look like disposable caches and are not:

| Path | Size (2026-11-10) | Why it must stay |
|---|---|---|
| `~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_*` | 2.7 GB | ESP-IDF 6.1 needs **GCC 15.2 RISC-V**; PlatformIO's copy is 14.2.0 and cannot build `esp32s31`. Registered in `~/.espressif/idf-env.json`. |
| `.scratch/esp-idf-6.1` (repo-relative `../../../.scratch/esp-idf-6.1`) | 2.7 GB | The ESP-IDF 6.1 checkout itself (`.git` is 2.1 GB). `export.sh` and every `idf.py` call read it. |
| `build/`, `build_reader/`, `build_shell/` | 0.7 / 0.5 / 1.4 GB | Per-variant configure trees. Each toggle of `CPYOS_UI_PROBE` / `CPYOS_READER_DEMO` / `CPYOS_SHELL_DEMO` needs a `reconfigure`, which discards the compiled objects of the previous variant, so keeping them separate is what makes switching cheap. `scripts/build_all.sh` writes into `build/` sequentially; the other two are manual trees. |

Rules that come out of that:

- Never copy a PlatformIO core directory into the repository to "make it
  writable". If a sandbox needs one, point `PLATFORMIO_CORE_DIR` at
  `/tmp/<name>` and delete it when the task ends: on APFS the copy shares
  blocks with the original, so `du` reports 7 GB but deleting it frees only the
  unique part (measured: 3 GB for a 6.9 GB clone).
- `~/.espressif/dist/` and `~/.platformio/dist/` hold download archives only and
  are safe to delete; they are re-fetched if a tool has to be reinstalled.
- `idf_tools.py uninstall` (or moving `~/.espressif` + `.scratch/esp-idf-6.1`
  to an external disk) is the supported way to reclaim the Plus toolchain when
  the port is not being worked on. Do it only with no `idf.py`/`ninja` running.
- Judge reclaimed space with `df -h /`, never with `du`: clones and hardlinks
  are counted per path.

PlatformIO-side note: `~/.platformio/tools/` and `~/.platformio/packages/` hold
the same tools twice on this machine. They are hardlinked together (see
`exp/dedupe_pio_tools.sh` at the workspace root, and AGENTS.md §38.9). Do not
`pio pkg uninstall` or replace those directories by hand.

First-power-on procedure and expected serial output:
[`docs/BRINGUP.md`](docs/BRINGUP.md). The consolidated pass/fail matrix is
[`docs/ACCEPTANCE.md`](docs/ACCEPTANCE.md).

## What runs on the device

BSP build (serial log, no UI):

```text
CPYOS-PLUS: Metalio E-INK4 Plus bring-up build - UNTESTED ON HARDWARE
CPYOS-PLUS: chip=... cores=... flash=... psram=...
MetalioEpd: EPDiy init E0470A14-AF 34Pin VCOM=1200 mV
CPYOS-PLUS: display bring-up pattern pushed (1216x684)
CPYOS-PLUS: FT6336U touch ready
CPYOS-SD: mounted 4-bit SD at /sdcard
PLUS-PWR: BQ27220 <mV> mV <mA> mA <soc>%
```

`CPYOS_UI_PROBE=1` instead draws a CPYOS-rendered status page in portrait
(684×1216 logical frame): build identity, panel size, last touch in logical
coordinates with a crosshair, the last tap/long-press/swipe/cover-key event,
SD mount state, and the BQ27220 reading. Every event redraws and pushes a
full-sheet GC16, which exercises rails + waveform + input in one loop.

`CPYOS_SHELL_DEMO=1` boots the activity shell and logs one
`CPRO: PSA self-test ok (AES-128-CBC + SHA-256)` line before the first frame —
that is the cheapest on-device proof that protected EPUBs can decrypt.

The shell also runs a **serial bring-up console** on the USB-Serial/JTAG stdin
(zero-timeout `select`, so it never blocks the UI). It is there so the first
hardware session can answer "which part is dead?" without a rebuild:

```
help
info                      chip / version / heap / PSRAM / uptime
batt                      BQ27220 mV/mA/SOC + CX25601N VBUS/charge state
rtc                       PCF8563 calendar + system UTC
sd                        mount state + first 12 root entries
imu                       SC7A20H detected? motion mode
touch                     FT6336U present, held ms
keys                      raw TCA9555 P0/P1 bits (power/volume active level and order)
slpinfo                   wake cause + GPIO2 (expander INT) + power-key/USB state; sleep is not wired yet
pinyin                    load the bundled pinyin database (syllable count) or report failure
hyphen                    load the bundled English hyphenation trie (bytes + language)
i2c                       scan the shared I2C1 bus and label expected parts
sdwrite                   4 KiB write/read/delete test on the SD card
haptic [ms]               pulse the motor directly (default 35 ms) to check polarity
panel [fast]              push a fixed test pattern (GC16 by default) over the frame
window                    push a 220x140 box; area refresh is disabled, so this is a full-frame push
screenshot                dump the live 1-bit framebuffer as a BMP over the serial line
crypto                    PSA AES-CBC + SHA-256 self-test
wifi                      scan, first 10 SSIDs with RSSI
apoff                      release the radio (SoftAP is refused: S31 beacon pool fault)
slptest [s]               real light-sleep park/sleep (USB console does not return; reset with esptool)
web [ssid] [pass]        start the SD file-transfer page (AP if ssid given); `weboff` stops it
radiooff                 stop the web server and fully release/deinit Wi-Fi
usbmsc on|off            expose the SD card as a USB drive (one-way; power-cycle after)
selftest                  info + battery + RTC + SD + IMU + touch + crypto + light
touchraw                  ten raw FT6336U samples (corner/cover-key mapping)
rtcset <epoch>            write UTC seconds into the PCF8563
light <0-100> [warm]      set frontlight brightness/warmth (vendor 10 % cap)
```
Cover keys map as **home/back → Back, prev → Up, next → Down** (the reference
Read Pico strip mapping).

**VCOM caution:** the vendor `config.h` uses **1200 mV**, and the port writes the
same value. The reference read-pico unit measured 3000 mV for a different panel
batch — do not copy that number without measuring this unit. A wrong VCOM can
permanently damage the panel; the serial line `TPS PowerOn ok vcom=<n> mV` is the
value being written. If the first frame looks wrong, power down instead of
changing other rail settings blindly, and keep the first sessions short.

## Flashing (UNTESTED — read the VCOM caution first)

No image here has run on hardware. **Never flash a Read Pico image**, and never
use `idf.py flash` on a stock unit: that command would also write this project's
bootloader/partition table and destroy the vendor layout.

### Safe path for a stock unit: bring-up in the vendor layout

The stock table is in `partitions_vendor.csv` (copied from CloudZao v2.0.74,
`partitions/v1/16m.csv`): `ota_0@0x80000`, `ota_1@0x580000`, each 5 MiB. The
bring-up (3.3 MB) and reader (2.4 MB) images fit; the full shell does not.

```bash
source $IDF_PATH/export.sh          # so python -m esptool resolves to IDF's

# inspect first: no serial write
python3 scripts/flash_metalio_plus.py --layout vendor --slot both --dry-run \
  --firmware ../../../../artifacts/cpyos_metalio_plus_bringup_20261109_untested.bin

# first hardware test: full backup, then write the app to BOTH stock slots
python3 scripts/flash_metalio_plus.py --layout vendor --slot both \
  --firmware ../../../../artifacts/cpyos_metalio_plus_bringup_20261109_untested.bin \
  --backup /tmp/plus-full-16m.bin
```

The planner preflights the chip (`ESP32-S31`), the 16 MiB flash, the eFuses
(secure boot / flash encryption must be off) and the real partition table at
0x8000; it refuses if the table is not the stock layout. If `espefuse` cannot
read the unit, confirm the eFuse state manually and only then pass
`--skip-efuse-check`. In this vendor layout the planner writes only the app
slot(s); `nvs`, `otadata`, `model`, `resources`, `font_data`, `coredump`, the
bootloader and the partition table are untouched. `--slot both` is the safe
default for the first test because the stock `otadata` may select either slot;
a single slot can be selected with `--slot ota_0|ota_1`.

★ **Cross-version boot is unverified:** the stock bootloader is IDF 5.5.4 while
this app is built with IDF 6.1 preview. If the first boot never reaches the
bring-up banner, restore the 16 MiB backup rather than flashing repeatedly. A
full install can later write this project's bootloader, but that is a separate,
riskier step.

**Data packs use the CPYOS spiffs layout, not the vendor one.** The stock
spiffs partitions (`model`/`resources`/`font_data`) are vendor-owned and the
CPYOS loaders read partition-relative offsets from the CPYOS spiffs base
(`0xCD0000` with the dual-OTA table, `0x9B0000` with the single-slot table).
The image runs without them, but the UI subset then lacks the common Han
reading face and the 33 packed languages fall back to English. Build and flash
them with:

```bash
python3 scripts/generate_hyphenation_db.py
python3 scripts/build_web_pack.py
# the pack the app build itself wrote (matches the port_i18n key signature):
cp ports/metalio_plus/build_shell/i18n_pack.dat build/i18n_pack.dat
# pinyin needs the pinned wheel:
python3 -m pip download --no-deps -d /tmp/pywheel pypinyin==0.55.0
python3 scripts/generate_pinyin_db.py /tmp/pywheel/pypinyin-*.whl
python3 scripts/generate_data_manifest.py \
    --partitions ports/metalio_plus/partitions_plus_ota.csv \
    --out build/data_manifest_plus.txt
python3 ports/metalio_plus/scripts/flash_plus_data.py --dry-run   # inspect
python3 ports/metalio_plus/scripts/flash_plus_data.py             # write
```

The manifest (also shipped with each release) covers pinyin, web pages,
hyphenation, the i18n pack, the emoji face and the 1.64 MB compressed reading
core at spiffs +`0x180000`; 63 KB of the 3.125 MiB partition stay free.
`flash_plus_data.py` refuses blobs outside the partition and never touches the
app/bootloader regions. `scripts/flash_plus_parity.sh` runs the whole bring-up
(app0+app1, the six blobs, the engine upload and the managed install); rebuild
`build/pdfmod-rv.bin` with `scripts/build_pdf_artifacts.sh` after every app
build, because the engine is paired to the app ELF SHA at `+0xb0`.

The current shell fits the **dual-OTA** table as well (6,088,592 B against the
6,684,672 B slot), because the UI font subset moved into app0 and the 8pt CJK
bitmap is no longer linked. The single-slot `partitions.csv` below is therefore
optional; it is still useful when a build needs the larger 6.25 MiB spiffs
partition. The release CI builds against the dual-OTA table.

### WebDAV / Calibre smoke test

The Plus file-transfer server speaks WebDAV on port 80 and the Calibre plugin's
WebSocket upload on 81. With the unit on the same LAN:

```bash
python3 ports/metalio_plus/scripts/webdav_smoke.py <device-ip>
```

It checks OPTIONS, PROPFIND depth 0, the forbidden 8.3/case aliases of
`/.crosspoint`, a PUT/GET/DELETE round-trip and that port 81 is listening.

### Release assets (what a Plus install needs)

Every `cpyos-plus-*` release carries, next to the firmware:

| Asset | Purpose | Where it goes |
|---|---|---|
| `cpyos-plus-<ver>.bin` | app image | app slot (CloudZao OTA writes this only) |
| `cpyos-plus-<ver>-partition-table.bin` | partition table for the full install (offset `0x8000`) | `flash_metalio_plus.py --partition-table`, and the web installer's manifest |
| `plus-pdfmod.bin` | MuPDF engine, paired to that image's ELF SHA | `/.crosspoint/pdf/pdfmod.bin` on the card (or the pinned **PDF engine** row on the Plugins page) |
| `plus-pdf-assets.bin` | font/CMap bundle for the engine | card root `/pdf_assets.bin`; installed at boot |
| `plus-pdf-engine-manifest.txt` | one manifest line for this exact tag | consumed by the in-reader updater |
| `pinyin.dat`, `web_pack.dat` … | data partition blobs, **Read Pico only** (published under their plain build names) | spiffs via `exp/flash_device.sh` / `data_manifest.txt` |
| `plus-pinyin.dat`, `plus-web_pack.dat`, `plus-hyphen.dat`, `plus-i18n_pack.dat`, `plus-emoji.dat`, `plus-cjk_core_8105.dat` | the same six blobs for **Plus**, published under a `plus-` prefix | spiffs via `flash_plus_data.py` / `data_manifest_plus.txt` |

★ **The prefix is not cosmetic.** `i18n_pack.dat` and `web_pack.dat` differ
between the targets (same size, different bytes), and 0.8.3 published both
targets' packs under the same names in one release, so the uploaded bytes were
whichever job finished last (Read Pico's i18n + Plus's web). A hash-checking
installer refused a pack; an unverified one wrote the wrong bytes. Since 0.8.4
every Plus blob is `plus-<build name>` and `scripts/plus_release_bundle.py`
re-verifies each file against the manifest digest while staging. The released
`data_manifest_plus.txt` names the prefixed files, and `flash_plus_data.py`
resolves them next to the manifest, so a downloaded bundle is self-contained:
`python3 flash_plus_data.py --manifest data_manifest_plus.txt`.

CloudZao OTA writes **only** the app slot: languages other than EN/ZH, pinyin,
hyphenation, the emoji face and the 8,105-glyph reading core stay absent until
the data partition is flashed once (and again whenever those blobs change).

### Deliberate differences from Read Pico (port audit)

These are design or hardware consequences, not missing ports; everything else
is meant to behave like Read Pico.

| Area | Read Pico | Plus | Why |
|---|---|---|---|
| Bundled games | 2048/sokoban/gomoku/minesweeper/sudoku/chess | not built (`CROSSPOINT_PLUGIN_RUNTIME`) | the upstream catalog swaps built-in games for the plugin runtime on this target; games compiled but unreferenced are gc-sectioned |
| File transfer | CrossPointWebServer: files, settings, plugins-run, WebDAV, Calibre mode | Plus `web_transfer`: files, its own `/settings`, browser-plugin runner (`/plugin` + `/api/plugin-jobs`), **WebDAV (11 methods)** and the **Calibre WebSocket upload on port 81**, plus USB MSC | same feature surface, different page names/UX; nothing is ported from CrossPointWebServer because the Plus httpd already implements the protocols |
| PDF engine | flash XIP `pdfmod` partition | SD file loaded into PSRAM at fixed vaddrs | S31 has no spare flash partition in the CPYOS table; PSRAM is larger |
| PDF reading font | embedded core carrier + UI subset | same, provisioned through the data partition | identical rule once `flash_plus_data.py` has run |
| Refresh features | DU/wave/band updates on the E0470 LCD path | panel driver provides its own waveform/refresh policy | different panel/board controller |

### Font tiers on the Plus

The Plus uses the same tier *mechanism* as Read Pico -- a 190 KB CJK UI subset kept
in PSRAM (no SD latency), a 25 KB non-CJK UI supplement, and a compressed reading
core in the data partition at spiffs +`0x15C000`, released to
`/fonts/CPYOSCJKCore/` on first boot -- but **not the same core tier**. The carrier
tier is chosen from the partition by `scripts/generate_data_manifest.py`: the Plus'
3.125 MiB spiffs holds the full 8,105-glyph carrier (1,640,306 B compressed →
2,555,704 B released, 211,086 B still free), while the Read Pico XIP layout's 2.0 MiB
only fits the 3,500-glyph one. `test/test_data_layout.py` fails if the Plus table
ever shrinks below 8,105, because that would cut Han coverage without failing a build.
See [../../docs/engineering/font-tiers.md](../../docs/engineering/font-tiers.md).

After the 2026-11-14 CRC fix the shell boot log reports:

```text
[INF] [SFP] ... EMBEDDED-V1 ready: 293152 bytes; faces load on demand (4-bit coverage)
[INF] [USF] UI script supplement ready: 49152 bytes
[INF] [CORE] carrier present: 1640306 -> 2555704 bytes
[INF] [PERF] InxRecent first frame: render=1080ms
```

Chinese renders correctly on the panel (user-confirmed). The
`[ERR] [TTF] Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not
compiled)` lines are expected: this port intentionally builds FreeInkFont
without autohint. Logs are also mirrored to `/.crosspoint/logs/boot.log` on SD,
which is the fastest way to inspect a boot without the UART adapter.

The reading core is released with a corrected mmap range (CPFZ header + deflate
stream) and the shell now consumes `FontCacheManager` missing codepoints on
stable pages, so a rare title or a font-extra pack name offers
`FontExtrasActivity(ReaderPrompt)` once per boot. The expected first-boot lines
are `[CORE] released the 2555704-byte core to /fonts/CPYOSCJKCore/...`,
`[SDREG] Discovery complete: 1 families` and
`[SDFS] font supplements: 1 families at <pt>`.

The core/extra UI faces are attached through `typography::roleFontsFor()` to
**every** role id at its size (`SMALL`, `UI_10`, `UI_12`, `CONTROL_18`,
`NOTOSANS_18`, `NOTOSERIF_12/14/16/18`, `NOTOSERIF_TITLE`, footer/estimate and
the `CJK_UI_*` faces). That is what lets direct role users — the JJWXC app's
book-detail hero title (`NOTOSERIF_18`), its list rows and the font-extras
manifest names — draw Han glyphs from the core instead of boxing them. The
attachment does not depend on `typography::apply()`'s preferred bindings, which
an SD vector family load clears.

The port's own hard-coded copy is part of the embedded UI subset:
`scripts/build_ui_font_subset.py` scans `main/`, `components/cpyos_app/` and
`components/cpyos_ui/` as well as `src/` and `lib/`. `吗 板 钮 类 诊` (power-off
confirmation, OTA and transfer pages) previously existed only in the reading
core, so they boxed whenever the card was missing or had failed to mount.

### Destructive path: CPYOS single-slot full install

`partitions.csv` is **not** the stock table. It gives one 9.625 MiB app slot at
`0x10000` and a 6.25 MiB spiffs at `0x9B0000`, so the shell fits and the compressed
8,105-character font core can live in spiffs. Using
it replaces the vendor table and the vendor data partitions:

```bash
# full backup is mandatory; the planner also refuses if otadata is initialised
python3 scripts/flash_metalio_plus.py --layout port --full-install \
  --backup /tmp/plus-full-16m.bin
```

This writes the app and the data packs first and the partition table **last**,
then reads the app header back.

If a single `write-flash` drops the USB-Serial/JTAG link (the bench unit does
after roughly 0.5 MB at 921600, 460800 and 115200 baud), use the chunked
recovery writer, which sends 64 KB per esptool invocation and retries a dropped
chunk:

```bash
~/.platformio/penv/bin/python scripts/flash_chunked.py \
  --firmware build/cpyos_metalio_plus.bin
# --app-only rewrites app0/app1 only after a full install
```
 It is **not** a first-flash default, and an
interrupted install is not resumable from the device — keep the 16 MiB backup
and use BOOT/download mode. The planner refuses when the stock `otadata` is
initialised, because a CPYOS table has no `ota_1`: if the bootloader selected
`ota_1`, the middle of the new app image would be booted. It never hand-writes
`otadata`; boot the stock firmware with `ota_0` selected first, or stay on the
vendor layout.

Without the language pack the UI runs on the embedded English/Chinese strings and
every other language is reported unavailable; without the emoji pack emoji fall
back to the system face's tofu; without pinyin/hyphenation the keyboard falls
back to ASCII and words are not hyphenated. All are supported degraded states,
not crashes. `build/i18n_pack.dat` is regenerated at configure time and must
match the firmware's key set; `build/emoji.dat` comes from
`scripts/generate_emoji_pack.py`.

The planner's host regression is
`python3 -m unittest test.test_flash_metalio_plus` (17 cases, including the
stock 5 MiB slot guard, the vendor data-pack refusal, a stale-table refusal,
both esptool verify wordings and a partition-table binary round-trip).

## Partition tables

| File | Layout | Use |
|---|---|---|
| `partitions.csv` | one 9.625 MiB `ota_0@0x10000` + 6.25 MiB `spiffs@0x9B0000` | CPYOS **full install only**; not the stock table |
| `partitions_plus_ota.csv` | dual `0x660000` OTA slots + 3 MiB `spiffs@0xCD0000` | Read Pico-compatible layout; the compressed core sits at spiffs+`0x180000` |
| `partitions_vendor.csv` | `ota_0@0x80000` / `ota_1@0x580000` (5 MiB each) + `model`/`resources`/`font_data` | read-only reference for the stock preflight; never written by the planner |

The CPYOS dual-OTA table is the release layout. `plus_ota` uses CloudZao
protocol 1.1 and writes the inactive slot; the generic CrossMux `OtaUpdater`
stays `CPYOS_OTA_DISABLED` because its manifests are Read Pico only. The CPYOS
table must not be mixed with the vendor firmware or the Read Pico table.

## Hardware cautions (vendor + reference firmware)

- **VCOM** — see above. Not calibrated, never claim it is.
- **Rail sequencing** — TPS65185 PWRUP/VCOM/WAKEUP and PWR_GOOD go through the
  TCA9555. The vendor code waits on PGOOD in milliseconds, which is why this
  project sets `CONFIG_FREERTOS_HZ=1000`.
- **SD** — 4-bit SDMMC on dedicated SDIO pads (not GPIO-matrix); the card is
  shared with USB MSC in the reference firmware, so always eject before flashing.
- **USB** — HS UTMI for TinyUSB MSC and USB-Serial/JTAG share the PHY; the
  **ANA_SW** analog switch on GPIO36 selects the route. It idles high (serial
  console) and `usbmsc on` pulls it low before installing TinyUSB. On S31 there
  is one OTG port and it is the HS UTMI, so esp_tinyusb models it as port 0
  (`TINYUSB_PORT_HIGH_SPEED_0` exists only on multi-port chips such as the P4).
- **Panel** — the reference firmware uses a 48-phase waveform and warns that the
  same drive count does not guarantee the same optical grey; verify on glass.
- **Battery** — BQ27220 SOC is uncalibrated; the vendor's own doc says to use
  voltage-derived estimates. Charge termination is CX25601N hardware (VREG
  4400 mV), not software.
- **GPIO2 is not a buzzer here.** The reused Read Pico profile maps an LEDC
  buzzer to GPIO2; on the Plus that pad is the TCA9555 interrupt, so the shell
  clears `BoardConfig::ACTIVE.audio.buzzer` at boot. Do not reinstate a buzzer on
  this pin; the Plus's audio path is I²S, which is not ported.
- **Motor and side keys are on the expander.** The motor is TCA9555 P0.3
  (active high, 35/50/70 ms pulses, vendor constants) and the volume keys are
  P1.0/P1.1 (active low, mapped to Up/Down). The BSP's shadowed output helper is
  used so the motor write never clobbers the TPS65185 rail bits. **Polarity and
  the key order still need a bench check**; if the motor runs inverted, swap the
  level writes in `HalGPIO::updateHapticFeedback()`.

## Provenance and licenses

- The CPYOS project license is in [`LICENSE`](../../LICENSE).
- The vendor `CloudZao/Metalio-E-INK4-Plus` project is **MIT**; copied files
  keep their MIT notice and are recorded in `THIRD_PARTY_NOTICES.md`.
- kiikoread files marked `LicenseRef-Kiikoread-Personal-Study-1.0` are **not**
  copied into this port. Apache-2.0 files may be used with their notices.
  `python3 scripts/inventory_kiikoread.py --tree <rc110 checkout>` regenerates
  the per-directory license inventory (engineering doc §2.6) and the full
  per-file appendix (`docs/engineering/metalio-eink4-plus-rc110-files.md`), and
  `scripts/tests/test_license_scope.py` asserts both that no Personal-Study
  header appears in the port and that no Personal-Study file was copied
  verbatim when the reference tree is present. `bash scripts/run_host_tests.sh`
  runs the whole host suite in one go.
- `epdiy` is LGPL-3.0-or-later; the vendored display backend must comply.
- The Plus refresh policy and its forced area drive are derived from
  **kiikoread rc110** (Apache-2.0 application code + the vendored epdiy's
  LGPL-3.0-or-later highlevel extension). Details in
  [`NOTICE-kiiKOREAD.md`](NOTICE-kiiKOREAD.md).
- Build-time managed components and IDF stacks (esp_tinyusb/TinyUSB for USB MSC,
  ESP-IDF NimBLE for BLE, `esp_http_server`) are attributed in
  [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md); their sources are not
  vendored in this repository.
- The kiikoread application tree is analysed entry by entry in [`docs/engineering/metalio-eink4-plus-rc110-app.md`](../../docs/engineering/metalio-eink4-plus-rc110-app.md) (`scripts/inventory_reference_app.py`; unclassified reference entries fail the host suite).
- The vendor application tree is analysed entry by entry in
  [`docs/engineering/metalio-eink4-plus-vendor-app.md`](../../docs/engineering/metalio-eink4-plus-vendor-app.md)
  (generated by `scripts/inventory_vendor_app.py`; unclassified vendor entries fail the host suite).
- The CX25601N register map used by `plus_charger.c` comes from the vendor
  project's MIT driver (`boards/common/power/cx25601n.cc`); only read-only status
  decoding was carried over, no charge-parameter writes.

## Roadmap to a real port

1. **HAL** — done: PCF8563 RTC + SNTP, BQ27220 fuel gauge, CX25601N status,
   4-bit SD ladder, FT6336U/cover/volume/power keys, motor, IMU, USB MSC
   routing, BLE HID, frontlight (vendor 10 % cap) and **light sleep** (vendor
   TPS/bus/expander/EXT1 sequence, wake reason from P1.7). **Remaining:** CPU
   DVFS (this S31 build accepts `esp_pm_configure` but never moves the clock)
   and the hardware BATFET cutoff. Deep sleep is deliberately not added: the
   vendor's "off" is the P1.3 pulse, not `esp_deep_sleep`. The light-sleep
   style/image rows are visible; only the deep-sleep rows stay hidden
   (`CROSSPOINT_PORT_NO_DEEP_SLEEP_SCREENS=1`). Open SD handles (the reader's
   Section) are parked and reopened across a light-sleep slice, so standby over
   a book sleeps too; `sdpark` self-tests that path without sleeping.
2. **Activity host** — done: `ActivityManager` runs the shared CPYOS activities
   with no `src/` fork. Networking: minimal HTTP file page, documented file API,
   WebDAV, port-81 Calibre upload and a framebuffer screenshot endpoint, the **official CPYOS HomePage** at `/`, **FilesPage** (`/files`, `/files/en`) and **FontsPage** (`/fonts`, upload/delete SD fonts via `/api/fonts`), `/js/jszip.min.js`, `/simple` (minimal page), a human-readable `/diag` page, and `/api/diag`. `/settings` shows the current values and now accepts `POST /api/settings`
(two-pass validation, credential keys refused); `/api/frontlight` applies live.
The transfer page refuses the open SoftAP on this image (S31 beacon pool fault); join a Wi-Fi network or use the USB-drive path.
   The File Transfer tile opens the native transfer/Calibre page described above.
   **Remaining:** the full multi-page CPYOS web UI (needs an Arduino `WebServer`
   shim); font-extra downloads are compiled but not
   device-verified.
3. **Reader** — 16-level text implemented; windowed scans and column bands are
   live (3 s watchdog; see `docs/engineering/partial-refresh-rollout.md`). The
   Read Pico 44-tick water-ripple engine is not used; the same setting drives the
   vendor's `MODE_SPATIAL_WIPE_GL` (ten GL16 bands, one phase apart). Continuous
   reading with fast page turn uses `MODE_DU`. Optical result still needs eyes.
4. **OTA / release** — CloudZao protocol 1.1 query is implemented
   (`plus_ota.cpp`), and the dual-OTA table holds two 6.375 MiB shell slots.
   `.github/workflows/cpyos-plus-release.yml` builds a tagged Plus image,
   publishes it to the CloudZao market, and creates the GitHub release. The
   generic CrossMux `OtaUpdater` remains disabled; no Plus release/tag has been
   created yet.

## Build artifacts (2026-11-09, untested)

| File | Size | SHA-256 | Offset |
|---|---|---|---|
| `cpyos_metalio_plus_bsp_20261109_untested.bin` | 394,256 B | `f2d61cf1…c3adb9e4` | app |
| `cpyos_metalio_plus_bringup_20261109_untested.bin` | 3,317,488 B | `d28cef17…4edb56e6` | app |
| `cpyos_metalio_plus_reader_20261109_untested.bin` | 2,446,144 B | `38a0c9d4…208b2d46` | app |
| `cpyos_metalio_plus_shell_20261109_untested.bin` | 9,283,936 B | `dc2db486…da9fc520` | app |
| `cpyos_metalio_plus_pinyin_20261109_untested.dat` | 349,500 B | `e8c031fa…e81feb23` | `0xE70000` |
| `cpyos_metalio_plus_hyphen_20261109_untested.dat` | 351,756 B | `7b5dd7df…903e8bb0` | `0xF00000` |
| `cpyos_metalio_plus_i18n_pack_20261109_untested.dat` | 345,504 B | `8c5ce5da…dd7d4e08` | `0xF70000` |
| `cpyos_metalio_plus_emoji_pack_20261109_untested.dat` | 126,544 B | `ab2852a1…7fad43d2` | `0xFD0000` |

**Offsets are layout-dependent.** In `--layout vendor` the bring-up/reader image
goes to `0x80000` and/or `0x580000`; the shell is not flashable there. The pack
offsets in this table are the **2026-11-09 single-slot layout** and are stale:
the current font-tier layout uses pinyin `spiffs+0`, web `+0x56000`, hyphen
`+0x90000`, i18n `+0x100000`, emoji `+0x160000` and core `+0x180000` (spiffs
base `0xCD0000` with the dual-OTA table, `0x9B0000` with the single-slot table).
Always read `build/data_manifest.txt`. The
planner chooses the correct offsets from the selected CSV and preflights the
device table, so do not copy the offsets into a hand-written command.

The six data blobs are written by `exp/flash_device.sh` equivalents for this
board (the port's `flash_metalio_plus.py`): pinyin at spiffs+0, web at +0x56000,
hyphenation at +0x90000, language at +0x100000, emoji at +0x160000 and the
compressed reading core at +0x180000. The packager validates them (language
key/signature match, emoji `EMJ1` header/length/CRC, `PYDB`/`HYPN` magic and
the space before the next slot) and refuses to bundle a stale or misplaced pack.

Package them (checks the `untested` label, the 9.625 MiB single-slot / 6.375 MiB
dual-OTA limit, the core CPFZ header/CRC, **that the
language pack was built for this firmware's key count/signature/content
signature** — a mismatch would silently drop 33 languages back to English — and
that the emoji pack's `EMJ1` header/CRC match its payload; both packs are
required. It also adds `UNTESTED-ON-HARDWARE.txt` + `SHA256SUMS` + the
project/third-party licences):

```bash
python3 scripts/package_metalio_plus.py \
  --artifacts ../../../../artifacts \
  --out ../../../../artifacts/cpyos-metalio-plus-0.0.0-untested.zip \
  --idf-version "$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo v6.1)"

python3 -m unittest test.test_package_metalio_plus   # packaging gates
python3 scripts/verify_metalio_bundle.py --bundle ../../../../artifacts/cpyos-metalio-plus-0.0.0-untested.zip

# prove the features are actually linked, not just compiled
python3 scripts/verify_port_features.py

# every host-side regression (license/rc110, packaging, flash planner, BLE, transfer):
bash scripts/run_host_tests.sh

# shell image + UART0 console only (the bring-up image has no command console;
# its USB-Serial/JTAG dies when I2C takes GPIO0/1, and it logs a heartbeat)
python3 scripts/bringup_serial.py --port /dev/cu.usbserial-XXXX --out /tmp/plus-bringup
```

These live in the outer, gitignored `artifacts/` directory. They are **not** a
release and are not referenced by any OTA manifest.
