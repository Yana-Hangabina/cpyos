# Local modifications to freeink-sdk, carried as a patch

`freeink-sdk` is tracked by this repository as a **gitlink** (mode 160000) pinned at
upstream commit `96de1be`, and the submodule is **not initialised**: the directory holds
a working tree with no `.git` of its own. Consequences, in order of how much trouble they
cause:

- Edits inside `freeink-sdk/` are invisible to every git command run in this repository.
  They cannot be staged, committed, reverted or reviewed here.
- `git submodule update --init` checks `96de1be` out **over** the working tree and
  destroys whatever was edited. Do not run it on a tree that holds un-saved work; run
  `apply.sh` afterwards if you must.
- Committing *inside* the submodule would produce a commit that exists only on this
  machine, and moving the gitlink to it would make the repository unclonable for anyone
  else. That is a worse trade than carrying a patch.

So the local work travels as `96de1be-local.patch`, applied with `./apply.sh`
(idempotent: it detects its own marker function and refuses to double-apply).

## What is in the patch

Two ports from the **rpr** project (read-pico-reader / kiikoread, Apache-2.0; local copy
under `.scratch/rpr/`), both display-side and both default-off or behaviour-preserving
unless asked for — plus one unrelated storage fix (3):

1. **TEXTTURN white-to-mid-gray early settle.** `e0470_textturn_soft_settle()`, called
   once at the end of `e0470_waveform_init()`, applies rpr's algorithm to the `from=15`
   column of the TEXTTURN table (swap phases only, never drop one; at most two phases).
   Revert on a panel that looks worse: delete that one call.
2. **Water-ripple page turn.** New `libs/display/EpdiyLcd/src/e0470_page_turn/` (engine
   ported from rpr, **not** verbatim: two local changes, both about memory),
   `E0470_APPLY_WAVEFORM` in the e0470 waveform module,
   four purely additive hooks in epdiy (`epdiy.h`, `render.c`, `render_context.h`,
   `render_lcd.c` — with no phase pointer attached they are exactly the old behaviour),
   and the pass-through chain `EpdiyLcd.{h,cpp}` / `FreeInkDisplay` / `EpdiyLcdDriver` /
   `PanelDriver.h`. The reader-facing half (settings entry, `EpubReaderActivity`, i18n
   keys) lives in this repository proper and is committed normally.

   The two local deviations from rpr, both because internal RAM is the scarce resource on
   this board and rpr's tree does not share that constraint:

   - **Internal hot window.** The 40 KiB phase table stays in PSRAM, and each tick copies
     only the band rows it will actually read into an 8 KiB internal window
     (`ensure_hot_window`), so no PSRAM row is touched once the scan is running. It is
     staged *before* `epd_poweron` and refused (returning
     `EPD_DRAW_NO_PHASES_AVAILABLE`, no scan, no baseline damage) if internal RAM cannot
     provide it, which lets the caller fall back to an ordinary push.
   - **Its lifetime is one animation.** `page_turn_run` releases the window when the tick
     loop ends, on success *and* on failure. It is deliberately not cached across turns:
     caching it means one ripple turn costs 8 KiB of internal RAM for the rest of the boot,
     because the only other release point, `epdiyLcdEnd`, runs on the deep-sleep path and
     leaving the reader never reaches it.

rpr's own frozen note applies to both: equal drive counts do not guarantee equal optical
gray, so each needs on-panel comparison, not just a host test.

3. **SDMMC bounce buffer aligned to the data cache line.** `SdmmcBlockDevice` kept its one
   4 KiB DMA buffer with `heap_caps_malloc`, which guarantees only word alignment, while a
   comment next to `readSectors` claimed `heap_caps_aligned_alloc` — the comment was simply
   false. `sdmmc_read_sectors()` asks `sdmmc_host_check_buffer_alignment()`, whose required
   alignment comes from `esp_cache_get_alignment()`: 64 B here, since this board's sdkconfig
   sets `CONFIG_ESP32S3_DATA_CACHE_LINE_64B`. A buffer failing that check is *not* rejected;
   `components/sdmmc/sdmmc_cmd.c` quietly allocates its own temporary DMA buffer and memcpys
   every chunk through it. So an unlucky heap could silently cost a 4 KiB alloc+free+copy per
   request, on a board whose largest free internal block is ~30 KiB. Now allocated with
   `heap_caps_aligned_alloc(kDmaBufferAlignment = 64, …)`, which costs nothing and cannot
   fail where the old call would not have. This file is on the **normal SD path**, not just
   USB-MSC, so it affects every SDMMC board including shipping `readpico`.

   Measured context, so nobody re-investigates: this is *not* a throughput fix for USB-MSC.
   The card is on a **1-bit** SDMMC bus at 40 MHz (`BoardConfig.h` read_pico SD entry
   `{38, 42, 44, PIN_UNASSIGNED ×3, 1}` — D1/D2/D3 are not wired), so ~4–4.5 MB/s, while the
   ESP32-S3's USB-OTG is Full-Speed only (64-byte endpoints, ~1 MB/s). USB binds by ~3.6×,
   and the bounce path was worth ~2% of the SD side. Worth doing because it removes a hidden
   allocation from a hot path and un-lying a comment, not because copies get faster.

4. **Windowed (area) refresh — `epdiyLcdUpdateArea`.** A fourth entry point on the EpdiyLcd
   C API that expands a 1bpp framebuffer into epdiy's front buffer and runs ONE differential
   push scoped to a rect, diffing and copying back nothing outside it. epdiy already had the
   area primitive (`epd_hl_update_area`); EpdiyLcd only exposed the full-screen push, so a
   host that changed a small part of the page had to re-push the whole frame. It refuses
   while the baseline is unknown (an area diff has no full rewrite to fall back on) and
   leaves the HV rails up (area updates usually arrive back to back). Plumbing:
   `EpdiyLcdDriver::displayWindow` overrides the `PanelDriver::displayWindow` default (which
   pushed the whole frame) and uses `EpdiyLcdRefresh::TextTurn` — GL16 with the all-hold
   diagonal — so unchanged pixels are not driven at all. The host-facing half lives in this
   repository proper (GfxRenderer/HalDisplay `displayWindow`, and the reader's per-minute
   status-bar clock tick) and is committed normally.

   Coordinates are epdiy framebuffer-native (1216×684 here; epdiy's rotation is the default
   `EPD_ROT_LANDSCAPE`, so `_inverse_rotated_area` is the identity). `GfxRenderer::displayWindow`
   therefore takes PHYSICAL coords too; the reader computes them by scanning the raw framebuffer
   for the changed-pixel bounding box after repainting only the status strip.

5. **End-of-push pixel-drive release (rpr rc95), rails kept.** `EpdiyLcdPowerHooks` gains an
   optional `releaseDrive` hook; `ReadPicoPower.cpp` implements it as clearing XOE on the
   FCI9555 (keeping the SY7636A HV rails and VCOM up), and `epdPowerOn()` now re-asserts XOE
   from its "rails already up" branch so the next push re-enables the drive. `EpdiyLcd`
   calls it at the end of every successful push (full frame, water-ripple, and the area
   update), so a still/unchanged frame is not left driven. The panel image is bistable, so
   dropping the drive is not visible; a null hook keeps the old behaviour (drive held from
   power-on). This is the faithful port of rpr's "release pixel drive even after
   unchanged/failed output"; rpr additionally holds the rails for an 8 s burst, which this
   board does not need (it already keeps them between pushes by design).
   ★ **Needs an on-panel page-turn check** (rpr's frozen note applies): this is a board
   power-sequence change and only a live scan can confirm the panel still drives. Revert =
   drop the `releaseDrive` hook from the config (or set it to nullptr).

6. **Deferred (asynchronous) frame scan — page turns overlap layout.** The panel scan used
   to block the render task for the whole waveform (~415 ms of GL16), so the next page could
   only start after the glass had finished. The scan now runs on its own task and the caller
   returns as soon as the frame is started.
   - **epdiy**: `epd_draw_base` is split into `epd_draw_base_prepare/start/wait`; an
     `epd_scan` task (core 0, 3 KiB internal stack) advances the 37 phases, sleeping on
     `frame_done`. New `epd_hl_update_*_start/wait`, `epd_hl_update_pending()` and
     `EPD_DRAW_SCAN_BUSY` (contract violation / task missing). `render_context` is a
     singleton: at most one scan may be in flight and the caller owns the join.
   - ★ **The `back_fb` copy-back moved from after the scan to after `start` returns but
     before the caller regains control.** It reads `front_fb` while it still holds this
     frame, so the caller may redraw immediately afterwards. Consequence: a scan failure is
     only known at `wait`, by which time `back_fb` has advanced — **the caller must rebuild
     the baseline with a full frame** (EpdiyLcd clears `baselineKnown` and forces Full).
   - **EpdiyLcd**: `epdiyLcdDrawAsync`, `epdiyLcdDrawGrayAsync`, `epdiyLcdWaitScan`,
     `epdiyLcdScanPending`. Async entries return true **only when a scan is in flight**
     (ripple, unchanged frames and failures complete inline and return false). Prefill is
     raised to 64 lines for the duration and restored in `waitScan` (same cushion as the
     ripple: layout on the other core widens ISR jitter). Every panel/framebuffer entry
     joins first — including `end`/`deepSleep` (dropping the rails would abort a scan).
     `epdiyLcdStashBase` deliberately does **not** join: it only writes the host's 1 bpp
     base copy, so the AA base stash and grey-plane rendering overlap the previous scan;
     the composition in `epdiyLcdDrawGrayAsync` joins.
   - **Kept synchronous on purpose**: water-ripple (44-beat engine), Full/clean, any
     `turnOff` frame, the 16-level commit (`turnOff` + no pending semantics), and the
     windowed area refresh (`epdiyLcdUpdateArea`, driven by the once-a-minute clock).
   - **PanelDriver/FreeInkDisplay**: `supportsAsyncGrayDisplay`, `displayGrayStart`,
     `displayGrayFinish`, `grayscaleCleanupIsNoOp` (all default to the old synchronous
     behaviour, so no other board changes), plus `_pendingGray` in the facade's single
     pending state. `cleanupGrayscaleBuffers` skips the join for a no-op-cleanup driver so a
     deferred grey commit keeps overlapping; the next display entry drains it.
   - **Reader**: `EpubReaderActivity` enables `overlapRefresh` on Read Pico for plain page
     turns (non-AA, no images). AA pages get the overlap from the deferred grey commit
     itself. The framebuffer is safe to redraw because the driver copies it into epdiy's
     4 bpp buffer inside `displayStart()`; the scan reads the difference image.
   - **Verification**: `libs/display/EpdiyLcd/test/host/transactions.cpp::checkAsync()`
     covers the return semantics, the double-start join, prefill raise/restore, no-change
     frames, failure → Full rebuild, `turnOff` staying synchronous, and the deferred grey
     commit. The host harness gained a macOS path (ld64 has no `--wrap`; the resource
     harness is skipped there and still runs on Linux).
   - ★ **Live risk to check**: the other core's layout/rasterisation can widen ISR latency
     and starve the line queue (`line buffer underrun occurred!` → `Deferred frame
     failed`). Prefill 64 is the mitigation. Revert the overlap without touching epdiy by
     returning `false` from `EpdiyLcdDriver::supportsAsyncDisplay()` (or dropping the
     Read Pico `overlapRefresh` branch) — both fall back to the previous blocking path.

7. **Refresh-path stabilization after the panel-latch regression (2026-10-19).**
   Long DU/partial-refresh chains could latch this raw panel's source/gate driver state:
   the framebuffer stayed crisp, but the glass showed a horizontal smear that no later
   refresh could clear (only a reboot). The final combination that stayed clean:
   - **FULL stays on the calibrated 36-phase GC16.** `E0470_FULL_WAVEFORM` (the untrimmed
     48-phase reference) is deliberately *not* used: this board's line timing is
     calibrated against the default table, and every-FULL 48-phase over-drove the panel.
     FULL also goes through the standard `epd_hl_update_screen_full`; never fabricate
     `back_fb`, because the waveform is a `(from,to)` transition table.
   - **16-level pages use full-page GL16, not TextTurn.** `epdiyLcdCommitGrayscale16`
     pushes `EpdiyLcdRefresh::Half` with a new `forceAllLines` flag, which calls
     `epd_hl_update_screen_full` with the GL16 table. Every line is processed, so the real
     optical state and `back_fb` stay aligned; TextTurn's held diagonal let them drift.
   - **No DU and no partial-area scans on Read Pico.** The firmware promotes every
     `FAST_REFRESH` to HALF and routes `displayWindow` through a full-page differential
     push. The multi-area API (`epdiyLcdCommitGrayscale16Areas` etc.) is present but
     currently unused; `epdiyLcdUpdateArea` remains available for hosts that can use it,
     but must not be used on this panel.
   - **Blocking scans only.** `EpdiyLcdDriver::supportsAsyncDisplay()` and
     `supportsAsyncGrayDisplay()` return false: the deferred/overlapped scan state was one
     of the paths that could leave the panel latched.
   - **Per-phase line-queue reset.** Before each scanned phase the two feeder queues are
     checked; if `current != last` the log names the queue and both are reset. A single
     disturbed frame can then no longer poison every later frame with out-of-order lines.
   Verification: `test/host/transactions.cpp` carries the waveform stubs and the
   async-prefill expectation now matches the shipped 96-line cushion.

## Upgrading an older tree

This patch grew from 20 to 26 files when items 6 and 7 landed; the idempotence marker
moved from `e0470_textturn_soft_settle` to `epd_draw_base_start` when item 6 landed. A working tree that still
carries the earlier version cannot be patched in place: `apply.sh` detects that state and
refuses. Restore the tree to `96de1be` first (re-checkout in a scratch clone, then
`rsync` the SDK content over `freeink-sdk/`, **never** `git submodule update --init` on a
tree with unsaved local work), then run `./apply.sh`.

## Provenance of the patch itself

Generated by diffing a pristine fetch of `96de1be` (fetched read-only into a scratch
clone; the working tree was never touched) against the modified tree, and **verified by
round trip**: applying it to a second pristine copy yields a tree byte-identical to the
working tree (`diff -rq` reports zero differences, over the same exclusion set).
Regenerate the same way if the SDK work changes:

```sh
git fetch --depth 1 https://github.com/0x1abin/freeink-sdk.git 96de1be6ce08eb732909e6e8149af8f892b9a2c5
# check the fetch out as sdk_pristine, copy the working tree as sdk_work, then:
diff -ruN --exclude=.git --exclude=.DS_Store --exclude=__pycache__ \
          --exclude=cff --exclude=psaux --exclude=ft_cff.c --exclude=ft_psaux.c \
          --exclude=ftmodule.h \
          sdk_pristine sdk_work > raw.patch
# strip the mtime that diff appends after a TAB on every --- / +++ line, and normalise
# the header back to the plain --exclude=.git form, so regenerating produces a
# content-only diff instead of touching all 25 headers:
sed -E 's/^(---|\+\+\+) (sdk_pristine|sdk_work)\/([^[:space:]]+)[[:space:]].*$/\1 \2\/\3/' raw.patch \
  | sed -E 's|^diff -ruN --exclude=[^ ]+( --exclude=[^ ]+)* sdk_pristine/|diff -ruN --exclude=.git sdk_pristine/|' \
  > 96de1be-local.patch
# then verify the round trip before installing it, with the SAME exclusions:
mkdir sdk_verify && tar -c -C sdk_pristine . | tar -x -C sdk_verify
patch -p1 -d sdk_verify < 96de1be-local.patch
diff -rq --exclude=.git --exclude=.DS_Store --exclude=__pycache__ \
         --exclude=cff --exclude=psaux --exclude=ft_cff.c --exclude=ft_psaux.c \
         --exclude=ftmodule.h \
         sdk_verify sdk_work
```

★ **Pass the `--exclude` flags literally, never via a shell variable.** Under zsh an
unquoted `$EX` is *not* word-split, so `diff` receives one long nonsense pattern, silently
excludes nothing, and you get a 72-file patch that looks plausible. Cost me one regeneration
to notice; the symptom is `grep -c '^--- ' 96de1be-local.patch` coming out far too high.

### What the exclusions are for

| Excluded | Why |
| --- | --- |
| `.DS_Store`, `__pycache__` | macOS and Python artifacts. `libs/assets/Icons/tools/__pycache__/gen_icons.cpython-312.pyc` differs from pristine purely because the generator was run locally; a `Binary files … differ` line for it must never enter the patch. |
| `cff`, `psaux`, `ft_cff.c`, `ft_psaux.c`, `ftmodule.h` | ★ **Carried by a sibling patch, not by this one.** On 2026-10-16 13:44 CFF/PSaux support was added to the vendored FreeType (upstream `96de1be` ships only autofit/base/gzip/psnames/raster/sfnt/smooth/truetype), gated behind `FREEINK_FONT_ENABLE_CFF`. That work travels as **`patches/freeink-sdk-cff/cff-otf.patch`** with its own `apply.sh` and README — apply both. Excluded here so that neither patch has to be regenerated when the other changes. |

The consequence to be honest about: **this patch is not a complete description of the
working tree.** It is a complete description of the working tree *minus* the CFF work,
which `patches/freeink-sdk-cff/` carries. Round-trip verification only holds under the
exclusion set above; with both patches applied the tree is complete.

Keep the `sdk_pristine/` / `sdk_work/` path prefixes: `apply.sh` relies on `-p1`
stripping exactly one component.

If the pinned commit ever moves, this patch must be rebased or regenerated; it is not
expected to apply cleanly to any other upstream state.
