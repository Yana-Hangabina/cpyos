# Partial-refresh rollout checklist

What this branch changes on the display path, how to verify it, and how to ship or
roll it back. Companion documents:
`docs/engineering/read-pico-refresh-stability.md` (mechanism, measurements, incident
history) and `docs/plans/2026-10-28-partial-refresh-uiux.md` (the full plan, screen
matrix, and the photo-gated decisions).

## 1. What changes, and where

**App tree (git-visible):**

| File | Change |
|---|---|
| `lib/hal/HalDisplay.{h,cpp}` | `ScanPowerGuard` (240 MHz for the whole scan), `waitForScanIdle` present barrier, `cleanWindow` (forced-full GC16 over a rect), `takeGrayScanCount` |
| `lib/GfxRenderer/GfxRenderer.{h,cpp}` | band path (diff -> windowed push), skip-on-no-change, ghost-debt accounting, local clean with dilated rect, refresh counters (`PUSHSTATS`/`REFRESHMAP`), area-push quarantine switch |
| `lib/GfxRenderer/ChangedBand.h` | byte-level change bounding box (host-tested) |
| `lib/GfxRenderer/BandPolicy.h` | debt thresholds, sweep cadence, local-clean threshold, rect dilation (host-tested) |
| `lib/Serialization/PersistableStore.h` | present barrier before every store write |
| `src/activities/UiListActivity.{h,cpp}` | list subtree opts into band updates; shared row-move hook |
| `src/activities/util/PhotoDemoActivity.{h,cpp}` | `CMD:PHOTODEMO` photo fixture |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | sheet band opt-in, close/move hooks, clock tick through `displayPanelWindow` |
| `src/activities/apps/**` | game activities opt into band updates |
| `src/activities/home/MyActivity.{h,cpp}`, `src/activities/home/FindBooksActivity.{h,cpp}` | the two hand-rolled home screens opt into band updates; `debugMoveRing()` per screen for the soak |
| `src/main.cpp` | the serial commands below |

**freeink-sdk (must travel as the patch, never as a submodule commit):**
`patches/freeink-sdk/96de1be-local.patch` (29 files) carries the atomic frame-start
claim, the CKV lap wait, prefill 112, the AA A/B switch, the windowed-scan clean API,
the scan-in-progress probe, and the extracted `line_mask.h`.

## 1.1 Dev -> public port status (2026-10-30)

Ported in this pass (tree builds; `pio run -e readpico` SUCCESS):

| Area | Files |
|---|---|
| Renderer core | `lib/GfxRenderer/GfxRenderer.{h,cpp}` (band path, local clean, epochs, box filter, DU flag, stats), `ChangedBand.h`, `BandPolicy.h` |
| HAL | `lib/hal/HalDisplay.{h,cpp}` (ScanPowerGuard, cleanWindow, waitForScanIdle, idle tick, push stats, gray counts) |
| SDK (patch) | `epdiyLcdCleanArea`, `epdiyLcdScanInProgress`, push counters; `patches/freeink-sdk/96de1be-local.patch` regenerated (29 files, round-trip 0) |
| Activities | `Activity.h`, `ActivityManager.{h,cpp}` (+ public's reader-standby helpers re-applied), `UiListActivity`, `LibraryListActivity`, `InxRecentActivity`, `MyActivity`, `FindBooksActivity`, `KeyboardEntryActivity`, `PhotoDemoActivity` |
| Assets/covers | `BookCoverLoader` (byte cache + path memo), `UITheme` (`_g2` -> `_g8` normalisation), `JpegToBmpConverter` (Gray8), `Epub` (`_g8` thumbnails) |
| Settings | `periodicFullRefresh` + `readerRefreshPages` split (public's `statusBarClockUpdate` kept), i18n keys `STR_PERIODIC_REFRESH` / `STR_REFRESH_INTERVAL` |
| Power | `armScanWatchdog` in `main.cpp`; Wi-Fi release and idle rails were already in public |

Second pass (2026-10-30, later): the **dev application layer now covers public** for everything
except `main.cpp` and the app directories kept local (`jjwxc/`, `weread/`, `reading-stats/`,
`pdf*`). That includes the reader (DU fast turns, overlay bands, cadence demotion), standby
band, `statusBarClockUpdate` (now also in dev), the display stack, covers, settings and i18n.
`main.cpp` keeps the debug-command difference by design; its non-debug power paths still need
a line-by-line comparison (last item below).

Third pass (2026-10-30, later): `main.cpp` non-debug content is aligned too -- the battery log
feature (`BATT`/`BATTLOG`/`NET` plus its once-a-minute sampler) was back-ported into dev, so the
only remaining differences between the trees are the debug command sets. Both trees build.

Fourth pass (2026-10-30): the serial diagnostics `refresh_soak.py` needs were ported into the
public `main.cpp` (`GRAYSTATS`, `REFRESHMAP`, `PUSHSTATS`, `CLEAN`/`CLEANMAX`, `BANDS`,
`AREAPUSH`, `BANDPUSH`, `NOSLEEP`, `PHOTODEMO`, `KEYBOARD`, `TYPE`, `ROWUP`/`ROWDOWN`,
`CLOSEPANEL`, `APPS`, `CADENCE`, `NOFULL`). The remaining dev-only commands are PDF/game/DU
experiments, not acceptance inputs.

Acceptance done (2026-10-30, public image flashed to both slots):

1. `exp/refresh_soak.py`: **36/36** (bands, local clean, keyboard, reader entry/sheet/close,
   gray Home round-trip, My/Find, no unaligned rects, no crash, no underrun).
2. Boot: `0.1.2-cpyos`, 33 languages / 1578 keys, SD at 40 MHz, **0 `[ERR]`**.
3. Idle CPU: one downclock to 80 MHz, no 240/80 flip (the every-pass `ScanPowerGuard` in
   `HalDisplay::idlePowerTick` was removed; that was the flip).
4. Branding scan after each merge (done for this batch).

★ Keep this gate for future merges: flash to both slots, run the §4 matrix plus
`exp/refresh_soak.py`, and confirm no `[ERR]` before publishing.

## 2. Build and verify

```bash
source ~/.zshrc >/dev/null 2>&1
cd repos/cpyos-public
gg ~/.platformio/penv/bin/pio run -e readpico > build/readpico-build.log 2>&1
grep -E '\[(SUCCESS|FAILED)\]' build/readpico-build.log | tail -1
python3 - <<'EOF'
import struct
d = open('.pio/build/readpico/firmware.bin','rb').read()
o = 0x20
print(d[o+0x30:o+0x50].split(b'\0')[0], d[o+0x10:o+0x30].split(b'\0')[0])
EOF
```

`project_name` must be `crossmux` (not `arduino-lib-builder`); see AGENTS §6.

Host suites (all take their own mutation step and must end in `OK`):

```bash
bash src/changed_band/run_changed_band_test.sh
bash src/line_mask/run_line_mask_test.sh
bash src/band_policy/run_band_policy_test.sh
```

Device regression (sets `CMD:NOSLEEP`, ends on Home, exits non-zero on failure):

```bash
~/.platformio/penv/bin/python exp/refresh_soak.py
```

Flash (app slots + data blobs; the script verifies hashes and the reserved bytes):

```bash
bash exp/flash_device.sh            # 完整刷写（含四个数据 blob）
bash exp/flash_device.sh --skip-data  # 只刷 app0，先比对数据分区
```

## 3. Device acceptance (serial, no photos)

| Step | Expect |
|---|---|
| `CMD:REFRESHMAP` | `bands=on area=on`, `clean=<default>`, `max=<default>`, `counter<n>/<interval>`, union coherent (`0,0 0x0` when idle) |
| keyboard: `CMD:KEYBOARD` + `CMD:TYPE` | entry one full push; every keystroke one band (48 px tall strip) |
| reader: `CMD:READ <book>` | one full push with one GC16; page turns one push each; sheet open one push; row move one band; `CMD:CLOSEPANEL` one push |
| gray dashboard: `CMD:APPS` from Home | one push, one GC16, source `override` |
| `CMD:PUSHSTATS` | `sweep_caller/override/cadence/local_fb` explain every GC16 |
| `CMD:GRAYSTATS` | Home entry = 1 gray scan, 0 page pushes |

## 4. Photo gate (the only open decisions)

Run `CMD:PHOTODEMO 0` then `CMD:PHOTODEMO 1`; photograph the glass after each
`PHOTO demo done` (the screen is left blank so residue shows). Also photograph a
long read (15-20 min) and one keyboard/sheet/game update. Apply the decision table
in the plan (§4.9): it maps each observation to the exact default and setting
semantics to change.

## 5. Shipping to the public tree

1. `bash patches/freeink-sdk/apply.sh` on the public tree (idempotent; its marker is
   `epd_draw_base_start`). Regenerate the patch from a pristine `96de1be` with the
   README's exclusion set if the SDK work changed.
2. Carry the app files from §1; keep the public tree's own power/rail changes.
3. Rebuild there and re-run §2's four suites (the host suites live in `cpyos/src`).
4. `python3 scripts/generate_data_manifest.py` if any data blob changed (none of
   these display changes do).
5. `python3 scripts/verify_release_artifacts.py`, then the release steps in the
   **public tree's** `docs/cpyos/release.md` (that file exists only there).

## 6. Rollback levers (no rebuild required)

| Lever | Effect |
|---|---|
| `CMD:CLEAN 0` | local clean off: the periodic sweep is a whole-screen GC16 again (ships **on** since the 2026-10-29 photo gate) |
| `CMD:CLEANMAX <5..100>` | dirty-union threshold for a rect clean (default 60) |
| `CMD:CADENCE on\|off [pages]` | the two Settings -> Display cadence fields (periodic full refresh toggle + page count), persisted |
| `CMD:NOFULL 0\|1` | session-only: demote every reader GC16 (cadence and chapter start) for ghost experiments |
| `CMD:PERF` traces | one `[PERF] <activity> first frame: render=… since switch=…` line per screen switch |
| `CMD:BANDS 0` | suspend band decisions: every push is a full page, no snapshot is written |
| `CMD:AREAPUSH 0` | block the windowed scan: `displayPanelWindow` degrades to a full push |
| `CMD:BANDPUSH 1` | keep the band decision but push full pages (bisect) |

Code-level rollback: set `s_bandSuspended = true` in `GfxRenderer.cpp` (§ the
defaults block) — that is the pre-band behaviour and has been soaked clean.

## 7. Known limitations

- The band rect is byte-aligned by construction (the diff is byte-granular); a
  caller passing a non-multiple-of-4 rect gets an `unaligned band` log, because the
  column mask assumes 4-pixel groups.
- The band snapshot is 104 KB of PSRAM for the renderer's lifetime (deliberately
  not freed on mode changes; see the use-after-free note in the stability doc).
- The scan watchdog task costs 3 KB of internal RAM while the reader is open; it is
  created on the first area push and restarts the device if a windowed push never
  returns.
- USB-MSC, PDF, XTC and image pages keep full-page pushes (gray or one-shot); only
  1-bit change-strips band.
- The legacy hand-rolled list screens (`My`, `FindBooks`, reading stats) are not
  opted in: they have no shared row-move hook, so each would need its own soak.
