# Read Pico refresh stability: full-screen horizontal smear

Status: stable on the tested Read Pico unit (2026-10-20, build `afeccc5b-dirty`
with the display changes below). The electrical mechanism is not instrumented;
the record separates what was measured from what is inferred.

## Symptom

After mixed use (rapid app/settings switching, long reader sessions, reader
overlays, then the JJWXC chapter list), the whole panel could show a horizontal
smear/ghosting on every row. The defect was not tied to one activity or one
list. Once present:

- a normal page refresh did **not** recover it;
- a manual FULL refresh did **not** recover it;
- only a reboot restored a clean image.

The same panel could run the exact same pages for a long time without the
defect, so it was probabilistic rather than a deterministic page-render bug.

## The decisive measurement: the framebuffer was clean

The firmware has a serial command that streams the raw 1-bit framebuffer
(`CMD:SCREENSHOT`, implemented in `src/main.cpp`). Capturing it while the glass
showed the smear and decoding the 103,968-byte buffer produced a perfectly
crisp UI: text, list rules, cover and layout were all correct.

That rules out:

- font/glyph rendering, dithering and layout;
- framebuffer corruption from drawing;
- a stale page cached by an Activity.

The fault is therefore in the **panel/scan path** (waveform drive, LCD_CAM
state, line feeder or their interaction), not in the logical image.

## What was tried, and what it proved

| Experiment | Result | Conclusion |
| --- | --- | --- |
| UI FAST/DU with a FULL every 6 pushes | Smear still appeared and survived the FULLs | DU is not the only trigger; periodic GC16 alone cannot recover |
| FULL with `back_fb` complemented to force every pixel through GC16 | Smear persisted; could reappear immediately after a clear | Fabricating `(from,to)` is wrong: the waveform is a transition table, not a universal reset. This was removed |
| FULL using the untrimmed 48-phase `E0470_FULL_WAVEFORM` | Still failed after many FULLs | The board timing is calibrated for the default 36-phase table; the 48-phase reference table over-drives the panel. Reverted |
| Rails off/on + 3-cycle clear on every FULL | Clear ran (logged) but the next FULL could re-latch it | The clear was not the missing piece; keeping it also made every FULL very slow. Removed |
| Native-gray partial-area commits (home cover + four icons) | Reproduced the smear; refresh still ineffective | Partial scans are unsafe on this raw LCD. The API remains in the SDK but the firmware no longer uses it |
| Whole-page 16-level with TextTurn (all-hold diagonal) | Home book cycling left changing regions ghosted; later pages could drift | Held pixels let the real optical state diverge from `back_fb`. Replaced by full-page GL16 with every line forced |
| UI FAST promoted to GL16 | Necessary but not sufficient on its own | Remove the weakest differential profile from system UI |
| `displayWindow` routed through a full-page differential push | Reader clock/popup paths no longer issue area scans | Kept |
| Async/overlapped panel scans disabled on `EpdiyLcdDriver` | Still failed in a build that had the other changes | The deferred-scan state was not the sole cause |
| 36-phase FULL (calibrated table) | Required part of the final combination | Keep the default table for FULL |
| Per-phase feeder-queue check/reset | No `line queue ... not empty` was ever logged, but the final soak stayed clean | Low-cost safety net; a single disturbed frame can no longer poison every later frame |
| Reader/toolbar overlay snapshot double store | Logged `BW buffer chunk ... already stored`; no visual failure in the clean soak | A real snapshot bug; a firmware guard exists but is not yet flashed/committed |

Two important negatives:

- no `Frame failed`, `Deferred frame failed` or `line buffer underrun` was
  present during the successful soak;
- no feeder queue was observed non-empty at a phase boundary, so the queue
  reset is a guard, not a proven root-cause fix.

## Working model

The evidence supports a **scan-state latch**, not ordinary pigment ghosting:

1. A non-full or stateful scan (DU, a partial-area update, a held TextTurn
   diagonal, or a disturbed feeder frame) leaves the panel or the LCD_CAM
   pipeline in a state where subsequent scans still run but produce smeared
   rows.
2. The framebuffer and the software baseline can still be correct, so the next
   refresh keeps sending the same correct image through the same bad state.
3. A reboot reinitializes LCD_CAM, the line queues and the panel rails; that is
   why only a reboot recovered it.

The exact electrical/state-machine point is not proven. In particular, the
successful soak cannot distinguish between "the queue reset fixed it" and the
"36-phase/blocking/GL16 combination has not yet hit the trigger again".

★ Updated 2026-10-28 below: the latch is the 693-pulse gate ring losing a lap
because the renderer's error path started the same physical frame twice. A
partial-area update was a *trigger* (long multi-phase scans + underruns), not
the intrinsic cause.

## 2026-10-28: the error path starts the frame twice (verified on hardware)

A CKV frame-boundary probe was added temporarily to `lcd_driver.c`. It records,
for every `start_ckv_cycles()`, whether the previous finite RMT loop would still
be running (`expected_end = start + 693 * line_time`). A second probe tags each
start as `frame`, `frame-error` or `clear`, and `CMD:INJECTERR` injects one
`EPD_DRAW_EMPTY_LINE_QUEUE` into the next prepared scan for a deterministic
reproduction.

**Pre-fix reproduction.** Entering the Library with a cold index (3.3 s of
`LIBIDX` work) makes a feeder underrun. Once `ctx->error` is set,
`lcd_calculate_frame()`'s error branch runs in **both** feeder tasks, and each
one calls `epd_lcd_start_frame()`. The two calls are ~50 µs apart; the second
resets the 693-cycle RMT loop at the start of the first one. Measurement over
one such burst:

```
CKV early restart: prev=693 cycles, overlap=11038us, starts=... early=1..64
line buffer underrun occurred!
Frame failed (1024); clean retry required
```

`overlap ≈ 11038 µs` is one full frame (11 088 µs) minus the 50 µs between the
two starts. The gate ring loses that frame's worth of CKV clocks; every
remaining phase of the update repeats the truncation (`early` grew by one per
phase), which is exactly the "gate-phase latch" the earlier experiments
described.

**Fix.** `RenderContext_t` now has `atomic_bool frame_started`, reset in
`prepare_context_for_next_frame()`. Both the error branch and the normal
trigger call `claim_frame_start()` (an atomic compare-exchange) and only the
winner calls `epd_lcd_start_frame()`. This keeps the error path's blank frame
(it still lets the panel complete its 693 clocks) while making it impossible for
two feeders to start the same physical frame.

**Post-fix verification (same hardware).**

- `CMD:INJECTERR` + one page turn: `line buffer underrun` and `Frame failed
  (1024)` still occur (the injected error is real), but `CKV diag: starts=256
  early=0`.
- The full mixed soak (Library / Home / Settings / 30 reader turns / Stats /
  My) hit natural underruns four times; every summary reported `early=0`, e.g.
  `CKV diag: starts=1280 early=0 max_overlap=0us gap_min=85us`. The frames still
  fail and retry, but the CKV train is never truncated.

**Temporary instrumentation (strip before shipping).** The `CKV early restart` /
`CKV diag` counters, the `epd_lcd_set_ckv_tag()` calls in `render_lcd.c`, the
`ckv_diag_*` block in `lcd_driver.c/h`, `epd_test_inject_error()` in
`render.c`/`epdiy.h`, `epdiyLcdTestInjectError()` and `CMD:INJECTERR` are
diagnostics. The atomic claim itself is the fix and should stay. Regenerate
`patches/freeink-sdk/96de1be-local.patch` from the cleaned tree, not from the
instrumented one.

**Underrun and flash (same session).** The latch fix does not by itself stop the
frame from failing; a feeder stall still underruns and the retry's clean-up is
the visible flash. Two sources were found and fixed:

- **Rapid page switching / 16-level scans:** with the old 32-line prefill, the
  first underrun was exactly at **line 32** and in a late phase (21–31 of
  36/37). 32 lines = 512 µs; the feeder stalls right after the frame starts.
  Read Pico now prefills **96 lines (1.5 ms)**, still inside the 64-slot
  per-thread queues and the `2*64-16=112` ceiling. The same aggressive soak
  (4 rounds of Home/Library/Reader + 20 page turns) then reported
  **0 underruns, 0 `Frame failed`, 0 early restarts**.
- **Cold Library entry:** `CpyTheme::drawPopup()` calls `displayBuffer()`, so
  the "rebuilding" popup starts a multi-phase scan, and `rebuildIndex()` / the
  Library render ran concurrently with it. The first underrun was at **line
  233** in phase 0. `LibraryListActivity::onEnter` now rebuilds the index
  **before** the popup push (the manual-refresh and delete paths already hold
  `RenderLock`; adding one in `onEnter` itself deadlocked, so it is an ordering
  fix, not a lock). A cold rebuild (271 ms) followed by the popup/list scans
  produced **0 underruns**.

The temporary CKV counters/tags, `CMD:INJECTERR`, `CMD:RM` and the error
injection hook were **stripped** before the final build. The permanent parts are
the atomic claim, the global CKV wait, prefill 96, and the Library ordering.
Clean build: `project_name=crossmux`, `version=d057b27c-dirty`, RAM 89,680 B,
Flash 6,783,370 B (90.0%). Artifact:
`artifacts/readpico_ckvclaim_clean_d057b27c_20261028.bin`. Host
`test_transactions.py` passes.

**Residual.** A mid-frame feeder stall longer than the 1.5 ms cushion (a long SD
or WiFi burst) can still underrun. The latch is still prevented by the atomic
claim + CKV wait, but the frame would retry and flash. A zero-underrun guarantee
needs a bigger queue (internal RAM cost) or moving heavy SD work off the display
path; treating the 96-line cushion as the tested envelope is honest.

## Final stable configuration

**Firmware (CrossMux):**

- `GfxRenderer::displayBuffer` promotes every `FAST_REFRESH` to
  `HALF_REFRESH` on Read Pico, for UI and reader pages; UI promotes every Nth
  push to FULL, where N comes from the user's refresh-frequency setting
  (default 6, bounded 2..16 — see Remaining risks item 6).
- `GfxRenderer::displayWindow` uses a full-page differential push on Read Pico,
  so the reader clock and popups never issue a partial scan.
- Rapid taps on the reader coalesce: turns queued while a render holds the
  lock (or inside the 200 ms gap) drain as state-only `pageTurn` steps to the
  final page with a single render (`pendingTurnDelta` in `EpubReaderActivity`);
  a spine crossing requeues the leftover until the section is rebuilt. Merged
  jumps skip the water-ripple animation. Input/state layer only — no waveform,
  scan or push-mode change; bursts issue strictly fewer scans than before.
- Home renders in one full-page 16-level pass: cover and tile icons get native
  gray, text/chrome stay 1-bit.
- The `storeBwBuffer` guard (keep the first complete snapshot instead of
  freeing/re-storing it) is written but not flashed or committed.

**freeink-sdk patch (`patches/freeink-sdk/96de1be-local.patch`):**

- `waveformFor(Full)` returns the default `E0470_WAVEFORM` (36-phase GC16);
  `E0470_FULL_WAVEFORM` (48 phases) is not used.
- FULL uses `epd_hl_update_screen_full`; the previous-frame baseline is never
  fabricated.
- `epdiyLcdCommitGrayscale16` uses `EpdiyLcdRefresh::Half` plus
  `forceAllLines`, i.e. full-page GL16 with every line processed.
- `EpdiyLcdDriver::supportsAsyncDisplay()` and
  `supportsAsyncGrayDisplay()` return false: every scan is blocking.
- Before each scanned phase, `lcd_do_update` checks both feeder queues and
  resets them; a non-empty queue is logged with its `current/last` indices.
- The multi-area gray API remains compiled but unused; the host routes
  `displayWindow` to a full page. `epdiyLcdUpdateArea` must not be used on this
  panel.

**Commits:** `dae772df` (remove DU/partial/complement paths), `c61b2a12`
(modern 2-bit icons, full-page home gray), `e865a05` (blocking scans, 36-phase
FULL, per-phase queue reset). The stable device build reports
`project_name=crossmux`, `version=afeccc5b-dirty`.

## Verification performed

- Raw framebuffer capture during the smear: crisp.
- Serial log soak: rapid app/settings switching, reader page turns, reader
  overlays, comments, WiFi, JJWXC entry, home cycles; no `Frame failed`,
  underrun or queue-drift messages.
- Host tests:
  `freeink-sdk/libs/display/EpdiyLcd/test/host/test_transactions.py`,
  `src/changed_region`, `src/font_manifest`, `src/waterturn`.
- The host test's async-prefill expectation was updated to the shipped 96-line
  cushion, and the waveform stubs cover the FULL table.

## Remaining risks and optimization space

1. **`storeBwBuffer` double store (highest-value pending fix).** Nested reader
   overlays can log `BW buffer chunk ... already stored`; the current code
   frees the outer snapshot and stores the current frame. A guard that keeps
   the first complete snapshot is written in `GfxRenderer.cpp` but has not been
   flashed or committed. Validate it with reader toolbar/panel/comment
   round-trips before enabling.
2. **Re-enable async only with a proven scan-state fix.** Blocking scans cost
   the A1 overlap (reader page turn speed). Async may be recoverable by
   resetting the feeder queues and reinitializing LCD_CAM per session, but it
   must be re-validated with the same soak; do not enable it just for speed.
3. **DU is not currently used on Read Pico.** It may be innocent relative to
   the final failure, but the evidence does not justify re-enabling it. Any
   re-test needs a long mixed soak and the queue/scan logs.
4. **Partial-area gray is not worth reviving.** Besides the latch risk,
   epdiy's area update still scans the full panel; it saves driven columns, not
   scan time. One full-page 16-level pass is simpler and faster than five area
   commits.
5. **Runtime recovery instead of reboot.** If the latch ever returns, add a
   "panel reinit" path: rails down/up, reinitialize LCD_CAM, 3-cycle clear,
   repaint the current page. This is the runtime equivalent of the reboot that
   always worked.
6. **Refresh cadence.** The refresh setting is now named tiers (Clear /
   Standard / Relaxed / Never; storage key `refreshProfile`, legacy numeric
   `refreshFrequency` values migrate in `fromJson`). Each tier maps to its own
   reader page cadence and UI push interval (`CrossPointSettings::
   getRefreshFrequency` / `getUiRefreshInterval`): Clear = every page / every
   2nd push, Standard = 15 pages / 6 pushes (default, identical to the
   historical behavior), Relaxed = 30 / 10, Never = no periodic reader full
   refresh / UI still sweeps every 16 pushes. The UI interval is injected
   every main-loop pass next to `setFadingFix` and can never be disabled — an
   unbounded GL16 chain accumulates residue that no later GC16 reliably
   removes. Change the tier mappings only with a measured soak.
7. **Flash headroom.** The final builds are ~99.5% full (about 31 KB free).
   Before adding more, remove unused area-gray APIs, the dormant gray-rect
   cleanup, and legacy icon arrays for icons now served by `inx_modern.h`.
8. **Other boards.** The DU/partial/async guards are Read Pico-specific; C3
   and other panels keep their existing paths. Do not generalize these changes
   without board-specific validation.

## Reproduction notes

- Decode a raw capture:
  `CMD:SCREENSHOT` returns `SCREENSHOT_START:<bytes>\n`, the 1-bit buffer
  (1216×684, 152 bytes/row), then `SCREENSHOT_END`. Render with bit 1 = white,
  rotate 90° for the portrait view.
- Useful log patterns:
  `UI push mode=`, `line queue ... not empty`, `Frame failed`,
  `line buffer underrun`, `BW buffer chunk ... already stored`.
- A refresh that does not restore the image while the raw framebuffer is
  crisp is the signature of this failure class; reboot before investigating
  rendering code.
