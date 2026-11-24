# Metalio E-INK4 Plus: v2.0.74 port vs vendor v2.0.79

Status: audit only (2026-11-17). This is not a claim that every vendor change was
ported, and it is not a release note.

## Scope and evidence

- Vendor checkout: `repos/metalio-e-ink4-plus`.
  - `HEAD` = `2b2c36a` (`v2.0.74版本`), the baseline the Plus port was derived from.
  - `origin/main` = `14aa1d0` (`v2.0.79版本`), with `874538e` (`v2.0.77版本`) in between.
- Vendor delta: `git diff --stat 2b2c36a..origin/main` is **230 files,
  +11,534 / -5,819**. It is not a driver-only patch.
- Port baseline: `ports/metalio_plus` keeps the vendor BSP/power/display pieces
  from the v2.0.74 line and deliberately keeps the older, known-painting render
  stack (the port's gap note records the 2.0.66 render rollback). CPYOS supplies
  its own UI, reader, network and data model above that BSP.
- Therefore **do not merge the v2.0.79 tree wholesale**. A whole-tree update
  would overwrite the port's proven display path with a different streaming/LCD
  stack and would replace the CPYOS activity layer with vendor LVGL screens.

## Major differences

| Area | Vendor v2.0.79 | CPYOS Plus port | Consequence / next step |
|---|---|---|---|
| Touch firmware | Ships `ft6336u_v04_app.bin`; upgrade protocol has retry/count and ID logging; boot target is V04 | Upgrade is **not ported**; no firmware image in the port; the measured unit reports V03 | Do not flash V04 blind. First confirm the unit's FT6336U revision and recovery path; then port the upgrade as a separate hardware task. A wrong touch image can leave the panel without touch until recovery |
| Sleep rails | `epd_board_metalio_eink4_plus_sleep_cut_rails()` cuts **only P5**; restore is P5 only. P14 is no longer cut in this path | Port cuts **P5 then P14** on sleep and restores **P14 then P5** (`vendor/board/display/epd_board_metalio_eink4_plus.c`) | This is a real electrical difference: P14 is shared by 4G, BT audio and the PA. The port's sequence was verified with its own light-sleep pass, but v2.0.79 intentionally stopped cutting P14. A/B the port's sequence on the unit before changing; do not assume the vendor change is cosmetic |
| Waveform trim | `my_waveform_trim.c` was rewritten to measure longest sequences first, then recompute into the destination; the 16x16 static `trimmed_seq_t` work table is gone | Port still has `static trimmed_seq_t seqs[16][16]` | Vendor change saves a large static internal-RAM table and should be low-risk to port, but it touches the panel waveform path. Treat as a separate `work/plus-v2079-bsp` change with a panel check |
| Power policy | v2.0.79 removes the idle→standby and standby→shutdown setting/logic from `power_policy`; power_hw/standby_lp also changed | Port has its own vendor light-sleep sequence (TPS sleep, bus hold, EPD off, expander mask, EXT1 wake, rail cut) and CPYOS power policy | The two policies are not line-compatible. Do not copy the v2.0.79 file over the port's `plus_*` light-sleep implementation without a battery-only regression |
| BLE page turner | Adds `ble_page_turner.cc` (~1,191 lines), `pager_screen` (~656), pager key handler/settings, Free2/3R/Smart1E/HBTR profiles | Port has its own BLE HID host, scan/map and reconnect attempts; no official pager protocol/UI; no physical peer | Functional gap, not a boot blocker. Pairing a real pager needs the vendor protocol decision and a peer; see the feature matrix |
| Cloud/books | `book_cloud_library.cc` (~178 changed), push parser (~263), cover thumbnails (~155), CDN/cloud screens changed | Port does not use the vendor cloud screens. It now has a separate developer-resource client; its book/badge/font paths are CPYOS paths | A direct vendor UI merge is not possible. Resource protocol is implemented from the 2026-11-16 developer docs, not by copying these files |
| Reader | `book_session.cc` (~640), `epub_document.cc` (~276), `txt_chapter.cc` (~138), `html_content.cc` (~122), page raster/prefs changed | CPYOS uses its own shared reader (EPUB/TXT/FB2/CBZ/XTC) and pagination | Feature-level parity, not code parity. Vendor reader improvements must be translated, not merged |
| Audio/assistant | WorkBuddy voice-stream changes (~237), audio service (~81), assistant boot photo (~947), assistant screen changes | Port has CPYOS AI-voice PTT, BT audio and local voice memo; no WorkBuddy cloud/assistant screen | Cloud activation/push is service-account dependent; not part of the resource API |
| UI/input | Screen keyboard (~437), network keyboard (~397), settings pager/power tabs, standby/wallpaper screens, home icons | CPYOS activity layer, keyboard, settings and themes | Not applicable as a source merge; CPYOS UI must stay authoritative |
| Data layout | `sd_data_layout.cc` (~409) and vendor `metalio/e-ink/{books,fonts,wallpaper,settings}` layout | CPYOS uses card-root resources and `/.crosspoint` state; resource downloads use `/books`, `/badges`, `/.fonts`/`/fonts` | Do not migrate the CPYOS tree to the vendor layout. The resource client uses the CPYOS paths and documents them |
| OTA | v2.0.79 `ota.cc` changed (~21 lines), vendor funnel remains service/account-driven | Port `plus_ota` now uses CloudZao protocol 1.1: `X-Resource-Key` + `Device-Id` at `/api/firmware-ota/check`, board `Metalio E-Ink4 Plus`, chip `esp32s3`, app `脆皮鸭CPYOS`; test channel adds `X-OTA-Channel: test` | Live test-channel query returns `0.3.1-plus-test1`; stable is 404 until published. The response's `has_update` is “firmware exists”, so the client compares versions. `X-OTA-Key`/`plus_ota_key.local` is retired. Publishing uses the authenticated community-firmware multipart API (release CI) |

## What is already equivalent enough

- Vendor BSP pin/power/display initialization from v2.0.74 is present and
  hardware-tested for boot, panel, touch, SD, frontlight, battery, RTC, IMU,
  audio, camera hand-off (module absent), light sleep and OTA-slot fallback.
- CPYOS feature-level coverage is tracked in `ports/metalio_plus/docs/plus-feature-matrix.md`.
  The new developer-resource client is documented in
  `ports/metalio_plus/docs/plus-developer-resources.md`.
- The resource API is **not present in the vendor open-source tree**: its
  endpoint table (`cloudzao_endpoints.c`) is blank in the public build, and the
  new `firmware-resources` strings do not appear in v2.0.79. The client was
  written from the published protocol instead of copying vendor code.

## Recommended order

1. **Safety / low-risk hardware deltas:** confirm touch V03/V04; port the
   waveform-trim static-table removal; A/B the P14 sleep-cut change. Each needs a
   real panel/battery pass.
2. **Pager:** port the vendor BLE pager protocol only when a peer is available.
3. **Resource service:** already code-complete for binding/list/download/ack;
   device-test with a real account and a mini-program push.
4. **Do not** rebase the whole port onto v2.0.79 or replace the CPYOS reader/UI
   with vendor screens. Keep the BSP delta isolated in a dedicated branch.
