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
the atomic claim, the global CKV wait, prefill 112, and the Library ordering.
Clean build: `project_name=crossmux`, `version=d057b27c-dirty`, RAM 89,680 B,
Flash 6,783,370 B (90.0%). Artifact:
`artifacts/readpico_ckvclaim_clean_d057b27c_20261028.bin`. Host
`test_transactions.py` passes.

**Residual.** A mid-frame feeder stall longer than the 1.5 ms cushion (a long SD
or WiFi burst) can still underrun. The latch is still prevented by the atomic
claim + CKV wait, but the frame would retry and flash. A zero-underrun guarantee
needs a bigger queue (internal RAM cost) or moving heavy SD work off the display
path; treating the 112-line cushion as the tested envelope is honest.

## 2026-10-28: the feeder budget is the CPU clock, not the cushion

The remaining overlay underruns had a single cause: **CPU frequency**. A panel
push that starts while the device is idle runs at `LOW_POWER_FREQ = 80 MHz`, and
at 80 MHz the two feeder tasks produce a line in ~28-32 us combined against a
16 us line clock. No prefill survives that: the cushion is consumed and the
frame underruns at line ~218-268, after which both feeders bail out and every
remaining line of that frame is empty.

Measured on hardware (each line is one physical frame):

| Condition | Result |
|---|---|
| 80 MHz, 10x `CMD:PANEL` overlay push | 10/10 `line buffer underrun`, first_line 262-268, prefill 112 |
| 80 MHz, same push with a held power lock (240 MHz) | 0 underruns, 6 pushes + 2 page turns |
| 240 MHz via `ScanPowerGuard` (final build) | 0 underruns, 0 `Frame failed`, 10 panel pushes + 5 water turns + Home |

The debug path is the honest reproduction: a serial `CMD:` is not "user
activity", so `HalPowerManager` drops to 80 MHz after 3 s and the push then runs
starved. A real finger tap raises the clock first, which is why interactive use
never showed it — but **every background push does**: the standby clock face,
the reader's minute clock tick, the buddy idle redraw. Any of those would have
underrunned, failed, and been retried as a clean GC16 (a flash) at 80 MHz.

**Fix (app layer):** `ScanPowerGuard` in `lib/hal/HalDisplay.cpp`, Read Pico
only. Every scan-triggering entry point (`displayBuffer`, `displayBufferAsync`,
`refreshDisplay`, both `displayGrayscaleBase` overloads, `displayGrayBuffer`,
`commitGrayscale16{,Area,Areas}`, `displayWindow`) holds a quiet
`HalPowerManager::Lock` for the duration of the scan. Quiet, because a real lock
(a network request) already implies 240 MHz. This is the only place that can
guarantee the frequency for *every* scan regardless of caller.

**Prefill is now 112** (`ReadPicoEpdiyConfig.cpp`), the `2*64-16` ceiling for
two 64-slot queues. It is the largest cushion that cannot deadlock both feeders
in `lq_current` (128 did). It buys latency jitter tolerance, not clock deficit.

### How much clock is enough (measured 2026-10-28)

Queued-but-unconsumed lines at 1/4, 1/2 and 3/4 of a 693-line frame, out of 128
slots (2 x 64). A feeder slower than the 16 us line clock burns this linearly,
so the three numbers show the margin:

| Scan-time clock | q1 / q2 / q3 cushion | Verdict |
|---|---|---|
| 240 MHz | 123 / 124 / 125 | feeders ahead of the clock; margin ~2.0 ms |
| 160 MHz | 108 / 106 / 106 | ~106 lines (~1.7 ms); deficit ~1.2% of the frame |
| 80 MHz (idle) | underrun at line ~262 | feeders ~2x too slow |
| 100 / 120 MHz | rejected by the S3 (`getCpuFrequencyMhz()` stays 240) | only 80/160/240 exist |

So **160 MHz is the intermediate robust value** on this SoC. The guard still
locks to 240 because the choice barely matters for energy: a full-page scan is
~0.41 s of wall time (37 phases x 11.09 ms) and the panel drive dominates it;
the 160-vs-240 delta is tens of mA for that window, i.e. ~0.001 mAh per push,
while 240 keeps 17 more lines of margin against an SD/WiFi burst landing inside
the scan. Energy is decided by *how many* scans and *how many pixels* each one
drives -- which is exactly what the partial-refresh work is for -- not by this
clock.

The feeder/queue diagnostics added for this investigation were stripped; the
underrun log keeps `mode`, `waveform_index` and `frame/cycle` so the failing
pass is still identifiable.

**Residual (unchanged in kind):** a mid-frame stall longer than ~1.5 ms (a long
SD or WiFi burst inside the scan) can still underrun. The latch is still
prevented by the atomic claim + CKV wait, but that frame retries and flashes.

## 2026-10-28: the windowed (area) push, and what "safe" now means

The reader's minute clock used to be routed through a full-page differential
push because partial scans had been implicated in the gate-ring smear. With the
underrun cause fixed, the exact area path was brought back — behind a watchdog.

**First attempt looked like a hang.** The first soak drove the clock through the
area path and then the unit stopped answering serial and esptool (port still
enumerated; `default_reset`, `usb_reset` and `no_stub` all failed) until a power
cycle. That build never actually scanned: `GfxRenderer::fillRect` takes LOGICAL
(rotated) coordinates, the test rect was written in physical ones, every tick
produced an identical framebuffer, `changedRegion` was empty, and each "tick"
only cost the ~70 ms of chrome drawing. The recorded incident is therefore **not
proven to be the area path**, and the device may simply have been idle; the root
cause is still unknown.

**A bounded harness settled it.** `CMD:AREATEST n` writes a strip straight into
the framebuffer (physical coords), pushes it through
`GfxRenderer::displayPanelWindow` -> `epdiyLcdUpdateArea`, restores the strip and
resyncs with a full page. Results on hardware:

| Run | Result |
|---|---|
| 1 push | 429 ms (a real 37-phase scan), returns normally |
| 10 pushes | 10/10, 428-429 ms each, no error |
| 50 pushes | 50/50, no underrun / `Frame failed` / CKV wait |
| real minute ticks (2 builds) | 4 ticks, windows 32x62 / 32x63 / 24x12, page unchanged outside the strip |

A captured framebuffer after 50 pushes + 2 page turns + 2 ticks shows the page
intact and the clock advanced.

**What the area push does and does not save.** epdiy still scans all 693 lines
for 37 phases (~410-430 ms); the rect only masks which pixels are driven. So it
does **not** shorten the scan. It removes the page-wide re-drive: the body keeps
its optical state, only the strip is electrically driven. That is the win, plus
a smaller HV current per update.

### Band updates: diff, then push only the strip

`GfxRenderer::setBandUpdates(true)` turns on a generic "push what changed"
path for opt-in 1-bit screens. While it is on, `displayBuffer()` compares the
framebuffer with the snapshot of the last pushed frame (PSRAM, 104 KB, allocated
on enable and freed on disable) and:

- no difference -> **no scan at all**;
- difference bounding box <= 55% of the panel -> `displayPanelWindow()` on that
  rect (watchdog armed), then refresh the snapshot;
- anything larger -> the ordinary full-page push, then refresh the snapshot.

The snapshot starts zeroed, so the first frame after enabling is always a full
push that also establishes the baseline. FULL pushes always take the full path,
so the periodic sweep still resets the panel.

The reader's Contents sheet bands too: a cursor move pushes the sheet column
(`band: 1008,46 192x598`, 14% of the panel) instead of the page. An earlier probe
reported "no change" for every move because the cursor had opened on the last row
and was clamped there -- check `panelIndex` before blaming the diff.

The first user is the keyboard: typing repaints the whole surface but changes
only the text line, so each keystroke now drives a ~48 px tall strip
(`band: 128,329 48x22`, growing to `48x459` as the line fills) instead of the
whole page. 20 consecutive keystrokes: 0 underruns, 0 `Frame failed`, 0 watchdog.
Closing the keyboard disables the mode before the screen underneath repaints.

Opt-in is `Activity::wantsBandUpdates()`; `ActivityManager::loop()` applies it for
whatever activity is on top every tick, so a push/pop transition sets the new
screen's mode before its first render (the reader never inherits the library's).
`UiListActivity` opts its whole subtree in (30 subclasses: language, network,
OPDS, plugin and settings lists): they are all 1-bit and none draws a grayscale
base (checked across `src/activities`), and a list that repaints the surface while
changing a viewport row is exactly the band case.

The two hand-rolled home screens opt in explicitly: `MyActivity` (tool drawer) and
`FindBooksActivity`. Both repaint in full and draw only an outline ring for the
cursor, so a ring move is a band. Measured with `CMD:MY` + `CMD:TAB 1` +
`CMD:ROWDOWN` and `CMD:FIND` + `CMD:ROWDOWN`:
`My` `136x298` (5%), `Find` `296x614` -> `392x614` -> `560x614` (22% -> 41%, the
union of the old and new rings; still under the 55% window). The first move after
entry is a band, not a page push, so the mode is applied before the screen's first
keystroke render (same entry contract as the keyboard). Each screen carries its own
`debugMoveRing()` -- `My` switches to the tool drawer first, because the record
chart and the settings dashboard have no cursor to move. `exp/refresh_soak.py`
phase C6 pins both (36 checks total).

Settings and the library opt in too. Measured on hardware, a **touch** list does
not draw a focus ring at all (`selectedIndex` stays -1), so moving the ring with
`CMD:ROWDOWN` leaves the framebuffer byte-identical: the useful outcome there is
`band: no change, push skipped` -- a redundant repaint now costs no scan (~430 ms
and one panel update saved). Only the scrollbar tick produced a real band.

Games use it too: one 2048 slide pushes `304x405` (10%) or `520x520` (25%) of the
panel instead of the whole page, and a Sokoban step pushes the one cell column it
touched (`560x88`, ~6%). Framebuffer captures confirm the board and the status
line are correct after band pushes.

The debt/sweep/local-clean arithmetic lives in `lib/GfxRenderer/BandPolicy.h` (three
thresholds: 10% to charge a band, 55% to keep a band windowed, 60% to clean locally)
and is pinned by `src/band_policy/run_band_policy_test.sh`: 99 checks including the
interval sweep, both sides of every threshold, the measured device cases (keyboard
48x459 and clock 24x12 charge nothing; the 2048 bands do), and the observed
sequence (five 2048 bands reach a 6-pass interval, the sixth cleans locally). A
mutation that lowers the debt threshold to 5% must fail. After the refactor the
device still cleans on the same pass (`fastCount 5 -> local clean 8,24 520x521`).

**Ghost debt follows driven pixels.** The periodic UI GC16 counter only advances
on a full-page HALF push, or on a band covering at least 10% of the panel
(`kBandDebtPercent`); a cursor row or the clock strip adds nothing. The "is the
sweep due" decision happens *before* the band attempt, so the pass that must
clean is never downgraded to a band.

Do **not** enable it on a page that draws a grayscale base: the base push and the
gray commit are two scans, and shrinking the first one makes them disagree about
what the panel holds. A band push invalidates the baseline only if it fails; the
snapshot has already advanced by then, so a later identical repaint would be
skipped until content changes -- self-healing, but stale in between.

The bounding box itself lives in `lib/GfxRenderer/ChangedBand.h` (no HAL
dependencies) and is pinned by `src/changed_band/run_changed_band_test.sh`: exact
rect cases plus an "every changed pixel is covered" oracle, and a mutation that
shrinks the bbox by one byte must fail the test.

The **column mask** that decides which pixels a windowed scan may drive lives in
`output_common/line_mask.h` (extracted from `epd_populate_line_mask`, still the
only caller) and is pinned by `src/line_mask/run_line_mask_test.sh`: 858 checks
covering the null case, even/odd range ends, every non-zero nibble value, a
200-trial apply oracle over random line data, and a full sweep of every range
(`[first,last)` for all even bounds) asserting the enabled set is exactly the
range. A mutation that swaps the two nibbles of a pixel pair must fail.

Writing the test surfaced that both conventions were undocumented in the code:
`dirty_columns` is one byte per two pixels with the **low** nibble as the first
pixel (`_interlace_line_unaligned` shifts by `4 * (x % 2)`), and the line mask
packs four pixels per byte with the **first** pixel in the low two bits. Both are
now written down in the header. What a host test cannot see is whether those two
orders also match the LUT's line packing. **That ambiguity is provably irrelevant
for this renderer's rects**: every band comes from a byte-aligned diff, so its x
and width are multiples of 8, hence multiples of the mask's 4-pixel group. A
group-aligned rect enables whole groups, and any within-group bit-order difference
permutes pixels inside a group that is enabled as a whole -- the same set is
driven either way. `displayBuffer` logs `unaligned band` if a future caller ever
breaks that (the AREATEST harness was moved to x=1144 for the same reason, though
it bypasses this guard). The windowed-vs-full-page photo comparison is therefore
no longer needed for correctness.

**AA waveform A/B (measurement only).** `CMD:AAWAVE 1` swaps the anti-aliased
gray push from the text-turn table (all-hold diagonal) to the real-diagonal GL16
table. Measured on hardware: with the water-ripple turn enabled, the gray commit
path (`drawGrayPage`) is **never called** -- the ripple engine composes the turn
from its own 40 single-phase frames -- so the switch is inert for that setting
and only matters for "turn effect = direct". Default stays on the text-turn table.

### 2026-10-28/29: the windowed push crashed — root cause found and fixed

Soaking the band path on games (and on a keyboard/games/reader mix) crashed the
unit with `Guru Meditation Error: Core 1 panic'ed (Double exception)` in the
ActivityManager task (the task that renders and pushes; 64 KB **PSRAM** stack).
Reproducer: Minesweeper + `CMD:GAMEMOVE <0..3>` every 1.4 s for 24 moves; the
crash rate was ~1 run in 3.

**Root cause: `Cache_WriteBack_All()` on a PSRAM-stack task.** The band path
called `GfxRenderer::flushFrameBufferCache()` before each windowed push (it was
added earlier as an underrun mitigation that later turned out to be a CPU-frequency
problem). On the ESP32-S3 that ROM call runs cache maintenance, and while it does
so the caller's stack in external RAM is not usable; the very next stack access
(or an interrupt saving context there) faults, and the fault inside the exception
handler becomes a double exception. That is exactly the crash signature: PC in
`_xt_context_save`, SP in PSRAM, a corrupted backtrace naming
`FreeInkDisplay::displayWindow`, and no ordinary stack overflow (61 KB free).

Bisect that pinned it (4-6 runs each, same reproducer):

| Configuration | Result |
|---|---|
| bands off | 0 / 3 crashes |
| band decision + full-page push + cache flush | crashed (1/3) |
| band + windowed push + cache flush | crashed (2/3) |
| band + windowed push, **no cache flush** (`CMD:BANDFLUSH 0`) | **0 / 4 crashes, 96 band pushes** |

Ruled out along the way: stack overflow (high-water mark ~61 KB free), the
FreeRTOS end-of-stack watchpoint (`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=n` still
crashed), a snapshot use-after-free (`setBandUpdates(false)` used to `free()` the
snapshot while a render could still hold it — fixed, but not sufficient), and an
I2C race (the bus has a recursive mutex).

**Fix:** `Cache_WriteBack_All` is gone, together with the four reader overlay call
sites of the same helper. Nothing needs it: every full-page push has always
written the framebuffer without it, and the band diff runs on the same core that
wrote it. Verified afterwards with bands and the windowed path **on by default**:
6/6 Minesweeper runs (144 pushes), 3/3 mixed keyboard/2048/Sokoban/panel/game
runs, and a real minute tick (`Clock tick: window=24,130 32x60`) — 0 crashes,
0 underruns, 0 `Frame failed`, 0 watchdog restarts.

The kill switches stay for future bisecting: `CMD:BANDS 0` (suspend band
decisions), `CMD:AREAPUSH 0` (force full-page pushes), `CMD:BANDPUSH 1` (band
decision, full-page push). `exp/coredump.py` dumps and unwinds the coredump
partition if another panic appears.

**Operational note: auto-sleep looks exactly like a hang.** Once auto-sleep fires,
USB-Serial/JTAG stays enumerated but stops answering (no serial, esptool cannot
sync) until the power key is pressed, and this board has no reset button.
**Confirmed 2026-10-28:** a unit that had been "dead" for 20 minutes answered
after a power press with `uptime` = 1,844,283 ms — it had slept, not crashed. Long
soaks must set `CMD:NOSLEEP 1` first, which suspends the auto-sleep timer.

### Local clean: a scheduled sweep over what changed instead of the whole panel

`GfxRenderer` keeps the union rect (physical coords) of everything pushed since
the last clean. The switch is on by default since 2026-10-29 (the photo gate in
`docs/plans/2026-10-28-partial-refresh-uiux.md` §4.9 found a rect clean and a
whole-screen GC16 leave the same residue; `CMD:CLEAN 0` restores the old pass).
With it on, a due sweep no longer turns the next push into a whole-screen GC16;
instead:

- the band decision still runs, and the union grows by the band rect;
- if the union is <= 60% of the panel, `HalDisplay::cleanWindow` runs **one forced-full
  GC16 over just that rect** (`epdiyLcdCleanArea` -> `epd_hl_update_area_full`),
  taking the content from the current framebuffer, and the counter/union reset;
- if the union is larger, the normal whole-screen GC16 happens and the union resets.

Because a forced-full rect rewrite drives every pixel in it, one rect GC16 both
displays this frame's content and cleans that area; nothing outside the union
changed, so nothing outside needs driving. An unchanged frame with a due clean is
also the quiet moment to run it (the "no change" path cleans and skips the push).

Measured (settings profile Standard, UI sweep every 6 pushes), 2048 board moves
produce 12-30% bands: after six of them the seventh logged
`local clean 8,24 520x521 of 1216x684` instead of a full-screen GC16. Three mixed
soaks (2048 + keyboard + minesweeper) with `CMD:CLEAN 1`: 2 cleans each, 0
crashes, 0 underruns. A framebuffer capture after two cleans shows the board and
chrome correct.

The optics were checked on glass on 2026-10-29: `PHOTODEMO 0` (two whole-screen
GC16) and `PHOTODEMO 1` (two dilated rect cleans) produce the same "faint, hardly
visible" residue with no dirtier rect edge, and a 9-page reading test showed only
a very light left-edge haze. That is why the switch now defaults on; the 60%
threshold is still a chosen bound, not a measured optimum (`CMD:CLEANMAX`).

### Present barrier

`HalDisplay::waitForScanIdle(timeoutMs)` blocks (bounded, 3 s) until no panel
scan is in flight. It is called at the start of every `PersistableStore::saveToFile`
(settings, stats, achievements, recents, app state -- all SD writes) and before a
cold library index rebuild. Rationale: a scan feeds one line every 16 us, and an
SD stall longer than the ~1.5 ms cushion is precisely what underruns a frame. The
barrier costs at most the frame already running, and it returns false with a log
on timeout so the work still proceeds (a stuck clean must not wedge the UI).

Non-ReadPico builds compile it to `true`; a panel that never reports a scan pays
nothing. Verified with three mixed soaks (library / settings / 2048 / reader page
turns / home): 0 crashes, 0 underruns, no barrier timeouts.

### Ghosting demo for the local-clean A/B (`CMD:PHOTODEMO <0|1>`)

The threshold decision needs on-glass comparison, so the comparison is scripted
instead of hand-driven. The command runs, in one blocking pass:

1. a test page from a clean GC16 -- a text line, a 50% dither block, a solid black
   block, and a row of one-pixel ticks (the ticks expose a mask that misplaces its
   rect by two pixels);
2. twelve board-sized updates (380x380, ~17% each, so every one charges ghost
   debt) with the local clean off (mode 0) or on (mode 1);
3. a blank white page pushed **without** a clean, so whatever residue the run
   produced stays on the glass for the photo.

Measured on hardware: mode 0 produced 10 bands + **2 cadence GC16 full-screen
flashes**; mode 1 produced 10 bands + **2 local cleans** (`480x500`, `480x596`)
and no full-screen sweep.

The clean rect is the dirty union **dilated by 8 px on every side** (clamped to
the panel, x/width kept byte-aligned, `band_policy::dilateRect`). A GC16 drives
every pixel inside the rect to its target identically to a whole-screen GC16, so
the cleaned pixels are optically the same by construction -- but the rect edge can
sit on the last driven pixel, where charge sharing with an untouched neighbour may
leave a faint boundary. Growing into already-clean pixels costs a few columns and
removes that risk. Measured on device: union `56,124 480x500` cleans `48,116
496x516`.

`CMD:CLEANMAX <5..100>` sets the dirty-union threshold for a rect clean at runtime
(default 60), so the same demo can be photographed at several thresholds without a
rebuild. Verified: at 90% the run did 2 local cleans; at 30% the second sweep's
union was too large and fell back to a whole-screen GC16 (`local_fb=1`) -- the knob
moves the policy exactly as intended. Both end with a framebuffer that is entirely white (0
black pixels), which is what the photo should show. Leaving the demo restores the
local-clean switch to off and band mode to whatever the next activity asks for.

### Device regression in one command (`exp/refresh_soak.py`)

`~/.platformio/penv/bin/python exp/refresh_soak.py` drives a fixed sequence over
serial and asserts what the host tests cannot see -- real pushes, real rects, real
counters:

1. twelve 17% board updates with the local clean off pay **cadence GC16s**;
2. the same sequence with it on pays **rect cleans** and no cadence GC16;
3. typing bands the text line and does **not** charge ghost debt (before/after);
4. entering a band-capable screen costs exactly **one** full push, and the first
   change after it is a band (the entry-path regression: a mode applied a tick late
   would make the first keystroke a full page);
5. the reader costs one clean push to open, one page push to open the sheet, a
   band per row move, and exactly one page push to close it -- none of it on the
   cadence (this is what `CMD:CLOSEPANEL` exists for);
6. a 16-gray page (the Home dashboard) moves through the *gray* entry points:
   exactly one gray scan, zero `displayBuffer` pushes, no cadence sweep -- the band
   work must never touch that path (`CMD:GRAYSTATS` counts the base/commit calls
   that bypass `displayBuffer`);
7. leaving the 16-gray dashboard costs exactly **one** push that is a full GC16
   whose source is the gray->1-bit promotion (`commitGrayscale16` sets the
   override), never the periodic cadence. A 1-bit waveform cannot erase the
   midtones a 16-level frame leaves, so this clean is required; making it local
   would cost a full 1-bit page push *plus* one rect clean per gray area, i.e. more
   scans for the same result, which is why it stays whole-screen;
8. `REFRESHMAP` reports the switches the run set, and `PHOTODEMO` restores them
   instead of clobbering the operator's choice;
9. no `unaligned band`, no crash, no underrun/frame failure anywhere in the log.

It sets `CMD:NOSLEEP` first (auto-sleep otherwise looks like a hang) and returns
non-zero on any failure. 32 checks, green on the current build.

Two behaviours it deliberately does not assert: a game may resume a board with no
valid move (no frame at all -- not a refresh bug), and the periodic cadence depends
on the user's refresh profile, so the script reads the counters rather than
assuming an interval.

### Policy state on demand (`CMD:REFRESHMAP`)

One line, no side effects, everything the refresh decision uses:

```
REFRESHMAP activity=Game2048 wantsBand=1 mode=1 bands=on area=on clean=off max=60% counter=1/6 union=8,24 520x484
```

`wantsBand` is the activity's opt-in, `mode` whether the renderer actually took it
for its current push, `bands`/`area` the two kill switches, `clean`/`max` the local
clean and its threshold, `counter/interval` the ghost debt, and `union` the rect
that a due clean would repaint. It answers "why did this screen not band?" without
a debugger.

Writing it surfaced a small bug: the full-page path zeroed only the union's width,
so the dump showed a stale x/y/height next to `w=0` (`128,319 0x41`). All resets now
clear the whole rect, and the dump reads `0,0 0x0` when nothing is pending.

### Refresh accounting (`CMD:PUSHSTATS`)

`GfxRenderer` now counts what actually reaches the glass: full-page pushes, the
subset that was a GC16 sweep, band pushes with their summed area, and pushes
skipped because the frame matched the last one. `CMD:PUSHSTATS` prints and clears
them, which is how the remaining partial-refresh work is measured instead of
guessed.

Tour on the fixed build (settings profile = Clear, i.e. a UI sweep every 2nd push):

| Action | full page | of which GC16 | band | skipped |
|---|---|---|---|---|
| boot first paint | 1 | 1 (caller) | 0 | 0 |
| open Library | 1 | **1** | 0 | 0 |
| Library row move x2 | 0 | 0 | 1 (384 px) | 1 |
| open Settings | 1 | 0 | 0 | 0 |
| Settings row move x2 | 0 | 0 | 0 | 2 |
| open reader | 1 | **1** | 0 | 0 |
| reader page turn (water) | 1 | 0 | 0 | 0 |
| reader sheet + 2 row moves | 2 | 0 | 0 | 1 |

With the source breakdown added (`sweep_caller / override / cadence / local_fb`),
the picture is calmer than the first tour suggested: an ordinary screen entry is a
HALF push, and the reader's entry is a single `override` GC16 (deliberate). The
earlier "every page push was GC16" came from session-accumulated entries that each
promoted a clean, not from the cadence. What remains is the cadence sweep, which
the local-clean path above replaces when enabled.

**The watchdog is part of the contract.** The area path has no bounded wait
inside epdiy, and this board has no reset button. Callers arm
`armScanWatchdog(3000)` around `displayPanelWindow`; a 200 ms polling task
restarts the device if the push does not return. The task is created on the
first arm, so nothing is paid until the area path is used. If the restart path
ever fires, the follow-up is a persistent fallback to the full-page push rather
than a reboot loop.

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
- The host test's async-prefill expectation was updated to the shipped 112-line
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
6. **Refresh cadence (two settings since 2026-10-29).** Settings -> Display now
   carries a toggle ("Periodic full refresh") and a page count ("Full refresh
   every (pages)", 5..120). The toggle is **off by default**: 60 differential
   pages on hardware left only the faint "thin paper" residue, so the reader no
   longer pays a whole-screen GC16 on a page count, and chapter starts are
   demoted with it (`ReaderRefresh`). The gray/image promotion
   (`nextRefreshOverride`) is a different path and still cleans. The UI sweep is
   independent (6 pushes at the default 15 pages) and, with local clean on, is a
   rect clean rather than a flash. `CMD:CADENCE on|off [pages]` sets the same two
   fields for device tests and persists them; `CMD:NOFULL 1` remains as a
   session-only override for experiments. Legacy `refreshProfile` tiers migrate
   in `fromJson` (Never -> off, Clear/Standard/Relaxed -> on at 5/15/30).
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
