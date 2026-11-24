# Font memory: what is resident, what streams, and what must never stall a scan

Measured on the Read Pico (2026-11-20, this worktree's image):

| Moment | PSRAM free | Notes |
|---|---:|---|
| after the dev-tree font tiering, before any network | **4,512,940** | `[NET] after font reclaim` |
| with a PDF open (engine mapped, page 1 laid out) | 4,520,472 | caches released before open |
| with a 2.5 MB SD CJK supplement copied resident | **503,820** min | the shape that made updates fragile |

The difference between rows 1 and 3 is a policy question, not a bug: copying a
whole TTF into PSRAM is only safe if nothing larger arrives later.

## What the tree already does (keep it)

- The core face ships as a **data-partition blob** (`cjk_core.dat`) and is
  reclaimed on demand (`NetworkStartup::setMode` / PDF open), not embedded in
  the app image.
- Reads go through `TtfEpdFont::addStreamSource` with a bounded prefix cache
  (1 MB reading / 256 KB preview), so glyph outlines can stay on the SD card.
- Glyph caches are size-capped per face; supplement families have a 4 MB total
  cap and a 512 KB contiguous-block gate.

## Rules this plan adds

1. **No whole-file resident copy above 1 MB.** A supplement (or a user font) is
   streamed, with tables (cmap/loca/hmtx) in the prefix cache. Measured cost of
   the alternative: 2.5 MB that later has to be reclaimed mid-session.
2. **Declare needs, do not sniff free memory.** Big consumers (PDF engine,
   browser 2 MB buffer, TLS session) already gate themselves; fonts must ask
   `psramHasHeadroom(need, cache, floor)` (the idiom emoji already uses) and
   degrade to streaming instead of taking the copy.
3. **A missing glyph must not abort.** Every new allocation on the font path is
   nothrow with an explicit fallback face; `PsramAlloc` aborting is how an OOM
   becomes a reboot.
4. **Never read the SD card while a panel scan is in flight.** The line queue
   holds about 1.5 ms; a slower SD read starves it and produces
   `line buffer underrun` -> `Frame failed` -> a white or torn frame (documented
   in section 35.6 and observed repeatedly). The write path already waits with
   `HalDisplay::waitForScanIdle()`; the streaming read path does **not**.
   Fill glyphs for a page *before* the push, and take the same wait at the start
   of a layout pass as a backstop.
5. **Unload only where it is provably safe.** The PDF module unload on reader
   exit was tried and reverted: it panicked (`xQueueSemaphoreTake ((pxQueue))`)
   because the render/page-cache path still held the mapping. Unload belongs
   where no document and no cached page can reference the module.
6. **Scenarios to keep honest:** Wi-Fi list (emoji + arbitrary scripts), WeRead
   and JJWXC content, book titles and authors (rare CJK). All are short strings
   against a cold cache, which is exactly where a per-glyph SD read is felt.

## Acceptance for any change here

- `SCANSTATS` on a forced full redraw: zero underruns, frame time within 10 % of
  the same page with the built-in face.
- Reader page turn with a streamed supplement: no increase in
  `Page render total` beyond 10 %, no `Frame failed` in a 30-page sweep.
- `[MEM] PSRAM Min Free` stays above 1.5 MB through: boot, open a PDF, close it,
  open the browser, run an engine update.
