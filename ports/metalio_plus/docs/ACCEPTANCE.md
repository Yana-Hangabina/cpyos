# Metalio E-INK4 Plus — acceptance checklist

Every item below is reproducible in this repository. **Sections 1–4 are green;
section 5 has been exercised on one unit and the per-feature evidence now lives
in [`plus-feature-matrix.md`](plus-feature-matrix.md).** The unchecked rows here
are kept as the original bring-up checklist; use the matrix for the current
state (verified / untested / blocked), because it is updated with every device
session.

## 1. Build gates (software)

| Check | Command | Evidence |
|---|---|---|
| Four variants build | `bash scripts/build_all.sh` | `[SUCCESS]`; images copied to the artifacts dir |
| App descriptor | `python3 - <<'PY'` (see engineering doc §0.4) | `project=cpyos_metalio_plus`, `version=0.0.0-untested`, size ≤ 0xE60000 |
| Reference target untouched | `pio run -e readpico` in `repos/cpyos-public` | `[SUCCESS]`, `esp_app_desc.project_name=crossmux` |
| Memory baseline | `idf.py --preview size` | DIRAM ≤ 60 %, ≥ 100 KB free; flash ≤ app slot |
| First-party warnings | `idf.py --preview build` | zero warnings from `ports/metalio_plus` sources (vendored libraries excluded) |

## 2. Host gates

| Check | Command |
|---|---|
| License + rc110 copy + vendored-attribution + inspiration checks (11) | `python3 scripts/tests/test_license_scope.py` |
| Packaging + flash planner + bundle/feature verifier + console doc tests | `python3 -m unittest test.test_package_metalio_plus test.test_flash_metalio_plus test.test_verify_bundle test.test_verify_features test.test_console_docs` |
| Feature presence in the built images | `python3 scripts/verify_port_features.py` (HTTP API, WebDAV, WebSocket, USB MSC, protected EPUB markers; reader/BSP must not carry shell-only services) |
| Vendor application inventory | `python3 scripts/inventory_vendor_app.py` (46 top-level vendor entries classified; an unclassified entry fails) |
| Native transfer/Calibre page wiring | `python3 -m unittest test.test_transfer_page` (activity in the activity layer, uses hooks, shell registers them, manager dispatches both entries) |
| Private JJWXC opt-in wiring | `python3 -m unittest test.test_port_jjwxc_wiring` (guarded `CPYOS_PLUS_JJWXC=1`, loud failure when `src/jjwxc` is missing, private tree gitignored) |
| Streaming upload / WebSocket / multipart / form / BMP / WebDAV helpers | `bash scripts/run_host_tests.sh` |
| rc110 file inventory | `python3 scripts/inventory_kiikoread.py --tree <rc110 checkout> --markdown ...` |

## 3. Bundle gates

| Check | Command | Evidence |
|---|---|---|
| Package | `python3 scripts/package_metalio_plus.py --artifacts ... --out ... --idf-version v6.1` | 4 images + i18n + emoji + legal + SBOM + SHA256SUMS + UNTESTED |
| Independent verify | `python3 scripts/verify_metalio_bundle.py --bundle ...` | `bundle OK`, 17 hashed members (4 images + 4 data packs) |
| Untested notice | unzip and read `UNTESTED-ON-HARDWARE.txt` | "UNTESTED ON HARDWARE", offsets, `bringup_serial` mention |

## 4. Flash gates (serial)

| Check | Command | Not yet |
|---|---|---|
| Dry run (stock layout) | `python3 scripts/flash_metalio_plus.py --layout vendor --slot both --dry-run --firmware <bringup.bin>` | prints `0x80000` and `0x580000`; no serial write |
| Refuse the full shell on stock | same with `<shell.bin>` | exits: 9.28 MB exceeds the stock 5 MiB slot |
| Refuse wrong image | point `--firmware` at a Read Pico image | exits with the project mismatch |
| Partition-table preflight | run against a unit | reads 0x8000; refuses a non-stock table |
| Full backup | `python3 scripts/flash_metalio_plus.py --layout vendor --backup /tmp/plus-16m.bin ...` | 16 MiB file + SHA-256 |
| Write + verify | `python3 scripts/flash_metalio_plus.py --layout vendor --slot both --firmware ...` | `verify_flash` passes for both slots |
| Destructive port layout | `--layout port --full-install --backup ...` | requires a backup; refuses initialised `otadata`; writes table+app+packs |

## 5. Device gates (unchecked until hardware)

The bring-up image shows the status screen and logs a 2 s heartbeat after USB
re-enumerates; record those lines and a photo for each row. `bringup_serial.py`
needs the shell image (command console) and a UART0 connection — it does not work
against the bring-up image over USB.

- [ ] VCOM measured and written deliberately (never assume the vendor value; the
      vendor BSP's own `measure_vcom` is a stub, so this needs an external meter)
- [x] TPS65185 rails + PGOOD; `TPS PowerOn ok vcom=1200`, 48-frame pattern pushed (software log 2026-11-09; optical image still needs a photo)
- [ ] 1-bit panel: optical check of the `panel` GC16 pattern (bars/lines); software scan passes
- [x] Window refresh: `areatest 20` performs 20 real `epd_hl_update_area` pushes
      (`err=0`, no watchdog trip); the band/window roll-out is described in
      `docs/engineering/partial-refresh-rollout.md` (2026-11-14)
- [x] I2C scan: 0x19 IMU, 0x20 TCA, 0x38 touch, 0x51 RTC, 0x55 gauge, 0x68 TPS, 0x6B charger all ACK (2026-11-09)
- [x] Touch corners/cover keys; tap zones page correctly (soak taps navigate the UI, 2026-11-12)
- [ ] Volume keys + power key; haptic feel at weak/medium/strong
      (35/50/70 ms; the vendor 70 % soft-PWM is not ported yet, polarity unverified)
- [x] Frontlight enabled: `light 80 40` -> 81/64 (vendor 10 % cap), user confirmed
      brightness/warmth; `plus_hw_check` asserts the duties
- [x] SD mount at 40 MHz 4-bit (2026-11-09); `sdwrite`/list/download/upload/delete
      covered by `exp/plus_verify_all.sh`; `sdpark` now verifies the light-sleep
      handle park/reopen without sleeping
- [x] Wi-Fi **station**: `esp_wifi_init`, retries (`reason=201` for a fake SSID)
      and a 19–20 network scan verified 2026-11-15 after releasing the audio I2S
      DMA (`NetworkStartup::prepare`). **SoftAP is refused**: the S31 library
      faults on its 752-byte beacon event buffer (`alloc eb` -> Load access
      fault). Full HTTP/WebDAV/Calibre transfer still needs a network with
      credentials (`plus_network_check.py`)
- [ ] USB MSC (ANA_SW low, host sees the card). Verified once (Mac mounted the card
      and took a 7 MB file); the eject path left USB dead once, so routine updates
      use SD flashing instead
- [ ] BLE HID scan/pair/forget; page-turn reports reach the reader
- [x] RTC clock seeded from PCF8563; battery 4368 mV / 100 % (2026-11-09)
- [x] Protected EPUB opens (PSA backend) and book keys survive a reboot
- [x] Screenshot: `screenshot` over serial and `/api/screenshot.bmp`
- [x] Light sleep over Home and over an open book, wake by power key and timer
      (2026-11-16: `usb=0` then `previous-wake reason=1 stage=14`; timer wake
      `reason=0 stage=14`). Real sleep current still needs a longer battery-only
      pass; the `浅睡` badge is the persisted `sleepStateBadge` setting
- [x] **Not enabled:** the CX25601N BATFET cutoff register stays read-only; the
      software 3.1 V guard + P1.3 pulse train is in place and needs a supervised
      battery session
- [x] Audio: `audio tone 300` -> 4800 frames, state=2, mic RMS 29–88
      (`exp/plus_audio_check.py` pattern), including after a Wi-Fi session that
      released the I2S channels. Speaker loudness and PA polarity still need ears
- [ ] Camera (GC2145): **no sensor on this unit** — the camera page reports
      `no ACK at 0x3c` and an I2C scan lists no 0x3c. The page degrades to
      `Unavailable` and re-initializes the panel; capture needs a populated module
- [ ] AI voice with a real xiaozhi URL/token; BT audio pairing with a headset
- [x] AI voice without a link: `voiceprobe` with the radio off prints
      `no network; connect Wi-Fi or 4G first` and the unit stays on Home. The
      worker checks `WiFi.status()` before any socket, because the radio release
      deinitializes lwIP; without the guard the first socket aborted in
      `tcpip_send_msg_wait_sem` and rebooted the board (2026-11-16)
- [x] AI key routing: an injected short key opens `PlusVoice` from Home,
      Settings and `EpubReader`; Back pops to that same page (reader page kept)
- [x] Microphone capture: `audio mic 30` -> `frames=30 rms=177` (ambient), mic
      path live at 16 kHz after the BT/voice work; after a full Wi-Fi scan +
      radio release, `audio tone 300` wrote 4800 frames and `audio mic 20`
      returned `rms=243`
- [x] Radio memory cycle: internal free 49,111 B during a scan, 73,023 B after
      `radiooff` (`NetworkStartup::prepare()` reclaims the font caches); no
      leak across a second start/release cycle
- [x] Page memory interactions (2026-11-16): a transfer/network session leaves
      ~56.6 KB free / 19.4 KB largest (one-time, not cumulative); BLE start+stop
      leaves ~56.5 KB / 15.4 KB largest because NimBLE teardown fragments the
      heap. The voice worker's 16 KiB guard therefore shows its low-memory
      message after a BLE session until reboot (graceful, no OOM); a fresh boot
      restores 73 KB / 27.6 KB
- [x] Storage park: `sdpark: read1=40 pos=40 park=1 restore=1 read2=40
      match=1 contiguous=1`
- [x] Cable-out (second run, 2026-11-16): 48.7 minutes on battery, then
      `previous-wake reason=power stage=14` (clean park/restore, power-key wake)
      and no panic. The 浅睡 badge was screenshotted through `CMD:SLEEPBADGE 1`
      (`artifacts/plus_light_sleep_badge_20261116.png`): the box draws at the
      bottom-right. The battery curve was not sampled because that run rebooted;
      `battlog` now persists `/.crosspoint/battlog` and resumed after a test
      reboot (`battlog: resumed 2 min`)
- [x] Embedded CFF font: a 5.6 KB CFF-subset EPUB opened and rendered page 0
      (518 ms) with `Embedded EPUB font: font @16pt` on 2026-11-15

## 6. Open items (not requirements met by a green build)

- Full CPYOS web UI: needs an Arduino `WebServer`/`AsyncWebServer` shim. The
  file API, WebDAV and Calibre upload are implemented without it.
- OTA: the single 14.375 MiB app slot cannot hold two images; updates are serial
  only until the layout is re-planned with hardware.
- I²S microphone/speaker via the external Bluetooth audio codec: vendor-specific,
  no CPYOS feature consumes it.
- Light/deep sleep: vendor S31 wake code relies on MPLL/PMU backports; do not
  guess it without a unit.

## 7. Hardware-gated acceptance (cannot run on this unit)

### 4G module
1. Insert a SIM, boot, run `cellprobe`; expect `present=1`, then Settings →
   Network → 4G and let it restart (the vendor saves then reboots).
2. `cellular` must print the IP, CSQ/dBm and `REG`; open the browser and WeRead
   over it. `WifiSelection` must not scan in 4G mode.
3. Play a sound cue (the BH1098 reset cuts P14) and confirm the `cell-recover`
   task re-initialises the modem; then start AI voice over 4G (needs a server).

### BLE page-turner
1. Enable the Bluetooth row, scan, pair the pager; the bond must persist across
   a reboot (`ready: 1 bonded`).
2. In the reader, the default map must turn pages (Left/Right/PageUp/PageDown);
   `btmap` shows the mapping, and the capture page shows raw codes.
3. Power the pager off and on: reconnect within 5 attempts (4 s apart); an
   explicit disconnect/forget must stop the retries.

### Eyes / measurement
- VCOM: `CMD:vcom 1200|1600|2000|3000`, reboot, compare black level; the
  default stays 1200 until a choice is made.
- Water ripple: Settings → Reader → page-turn effect → 水波纹, or `CMD:WIPE 1`;
  check the look on the glass.
- Haptics: weak/medium/strong feel (35/50/70 ms); the vendor 70 % soft-PWM is
  not ported.
- Battery-only: the cable-out run plus `analyze_power_log.py` gives the current
  curve; a supervised low-voltage/shutdown run still needs the pack datasheet.

A release is only "supported" when sections 1–4 are green **and** section 5 is
fully checked on the target hardware; until then every artifact keeps the
`0.0.0-untested` label.
