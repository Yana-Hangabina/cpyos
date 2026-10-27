# Deferred (asynchronous) panel push

Status: implemented on Read Pico, host-tested, simulator-checked; **not yet verified on
the physical panel** (2026-10-17). This document is the contract the display stack now
depends on; read it before adding another display entry point.

## Why

The epdiy LCD path is a raw parallel panel: every frame is clocked line by line by the
MCU, and `epd_hl_update_*` blocked the caller for the whole waveform (37 GL16 phases,
about 415 ms). The render task therefore could not start the next page until the glass
had finished, and every page turn paid layout + scan serially.

The scan reads the *difference image* in PSRAM; it does not read the caller's 1 bpp
framebuffer. So the framebuffer is free as soon as the driver has expanded it and built
the difference. That is the overlap this change exposes.

## Layers

```
EpubReaderActivity   ── displayBufferAsync / displayGrayBuffer
        │                (overlapRefresh on Read Pico, AA commit normally)
        ▼
FreeInkDisplay       ── _refreshPending (+ _pendingGray) and syncPendingAsync()
        ▼
EpdiyLcdDriver       ── displayStart/displayFinish, displayGrayStart/Finish
        ▼
EpdiyLcd             ── epdiyLcdDrawAsync / DrawGrayAsync / WaitScan
        ▼
epdiy highlevel      ── epd_hl_update_*_start / epd_hl_update_wait
        ▼
epdiy render         ── epd_draw_base_prepare/start/wait + the epd_scan task
```

## The contract

1. **One scan at a time.** `render_context` is a singleton. `epd_draw_base_start()`
   returns `EPD_DRAW_SCAN_BUSY` if a scan is still in flight; every EpdiyLcd entry calls
   `waitScan()` first, so callers normally never see it.
2. **After `displayStart()`/`displayGrayStart()` returns true the caller may redraw the
   framebuffer.** The driver copied it into epdiy's 4 bpp buffer and built the difference
   before returning. It must call `displayFinish()`/`displayGrayFinish()` (or any other
   display entry, which joins) before the next push.
3. **The return value means "a wait is owed".** Ripple, unchanged frames and failures
   complete inline and return false; the facade then leaves `_refreshPending` clear.
4. **`back_fb` advances at start, not at wait.** The copy-back runs after `start` returns
   but before the caller regains control, so it still sees this frame's `front_fb`. A
   scan failure is therefore only known at `wait`, and the caller **must rebuild the
   baseline with a full frame**. `EpdiyLcd` clears `baselineKnown` on any deferred
   failure, which promotes the next push to Full (clear + GC16).
5. **Kept synchronous on purpose**: the water-ripple engine (44 beats), Full/clean
   frames, any `turnOff` frame (the rails must not drop mid-scan), the 16-level commit,
   and `epdiyLcdUpdateArea` (the once-a-minute status-bar clock).

## Overlap and pacing

- Prefill is raised to 96 lines (1.5 ms) for a deferred scan and restored in `waitScan()`.
  ★ **Never raise it past the feeder queues' combined capacity.** Each feeder's line queue
  holds 64 entries; the start trigger must fire before both fill, otherwise both feeders
  spin in `lq_current`, the trigger line is never handed out, the frame never starts, the
  CPUs spin and the TWDT reboots the device. 128 did exactly that on hardware when opening
  the reader settings. `render_lcd.c` clamps to `2 * queue size - 16` (=112) as a second
  line of defence; underruns are additionally mitigated by joining before a font prewarm.
- Page-cache prewarm runs 10 ms after a render (not 400 ms) and builds only the forward
  slot. Two slots at ~700 ms each (~1.4 s) never finished inside a normal ~1 s turn, so
  the pass was cancelled every time and the cache almost never hit.
- The AA path stages its base and selector planes without joining
  (`PanelDriver::asyncSafeGrayscaleStaging()`): those are the driver's private copies and
  only the commit (`displayGrayBuffer`) reads them. Gray-plane rendering therefore
  overlaps the previous scan; the commit joins what is left. Font prewarm is the
  exception — it can read internal flash, which disables the caches, so the reader joins
  the scan before a prewarm (`needsPrewarmScan` runs once per font switch).
- The reader enables `overlapRefresh` for plain Read Pico page turns (`!pageHasImages`).
  Anti-aliased pages overlap through the deferred grey commit instead: AA pages stash the
  BW base (no join), render the two selector planes while the previous scan runs, and
  `displayGrayBuffer()` starts the next scan.
- `epdiyLcdStashBase` intentionally does not join; it only writes the host's 1 bpp copy.

## Cost

- One task, `epd_scan`, core 0, 3 KiB internal stack, created in `epd_renderer_init` and
  deleted in `epd_renderer_deinit`. It sleeps on `frame_done` between phases.
- No new buffers: the scan reuses the existing highlevel framebuffers.
- Static RAM grew by ~40 B (pending-state flags); Flash by the new code paths only.

## Verification

| Layer | How |
| --- | --- |
| epdiy split + state machine | `freeink-sdk/libs/display/EpdiyLcd/test/host/test_transactions.py` (`checkAsync`, highlevel semantics) |
| Facade + driver | same host binary (EpdiyLcdDriver compiled in) |
| Shared firmware paths | simulator build + scripted page turns (async is not exercised there: the simulator's `HalDisplay::supportsAsyncRefresh()` is false) |
| Xtensa build | `pio run -e readpico` (SUCCESS, `esp_app_desc.project_name == crossmux`) |
| **Physical panel** | **pending** — watch for `line buffer underrun occurred!` / `Deferred frame failed` and compare turn latency |

## Reverting

- Fastest: return `false` from `EpdiyLcdDriver::supportsAsyncDisplay()` — every deferred
  path falls back to the old blocking behaviour, including the reader's `overlapRefresh`
  (`supportsAsyncRefresh()` becomes false).
- Or drop the Read Pico `overlapRefresh` branch in `EpubReaderActivity::renderContents`;
  AA pages would still use the deferred grey commit.
- The epdiy split itself is behaviour-preserving for blocking callers (`epd_draw_base`
  is start+wait), so it can stay.
