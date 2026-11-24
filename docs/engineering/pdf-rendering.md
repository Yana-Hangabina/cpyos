# PDF rendering on Read Pico: memory findings and fallback ladder

Status: work in progress. This note is the system of record for the PDF render
budget so the next round does not re-derive it from serial logs.

> **Final status (2026-11-18):** the delivered engine is the flash-XIP `pdfmod`
> module documented in [`pdf-xip.md`](pdf-xip.md).  Out-of-core embedded fonts
> removed the font-buffer peak, and the reader-settings reflow now recovers
> pages 19 and 41 of the 112-page book (57 and 59 lines, three screens each).
> The “still fails” measurements below are kept as the historical record of why
> the fallback ladder, grayscale JPEG and out-of-core fonts exist.

## Device budget (measured, not estimated)

| Item | PSRAM |
|---|---:|
| Total | 8,373,520 B |
| Free after panel init | ~4.56 MB |
| Free after `pdf_open_document` (112-page book) | ~5.26 MB |
| Free after releasing external font pack + pinyin + hyphenation | ~5.34 MB |
| Free after `PdfDocument::load()` prep reset | ~5.82 MB |

`PdfDocument::renderPageToBmp()` reopens the document (context reset, ~0.6 s)
before a page when free PSRAM is below `maxWidth*maxHeight + 4 MB`, because a
rendered page keeps ~2-4 MB of decoded image tiles alive even after
`fz_drop_page()` + `fz_empty_store()` + `fz_shrink_store(ctx, 0)`. Only
destroying the `fz_context` returns that memory.

## Why the original tiled render failed on the 112-page book

Tiles ask the draw device for each tile's subarea, so MuPDF caches one decoded
image tile per subarea: page 68's first 128-row tile alone consumed 2.0 MB, the
second tile then ran out. A single full-page pass decodes each image once and
costs only the output pixmap (674x873 = 588 KB). Full-page is now tried first;
tiling remains only for low-memory starts.

## The glyph cache is the other hog

`fz_purge_glyph_cache()` after every band is what made page 18 render: its
memory had been growing through the band and a later buffer growth then failed.
With the purge, page 18 renders **at 100 % with images** and PSRAM stays flat
(4,995,936 B free after the first band and after the last). MuPDF caps the glyph
cache at 1 MB (`MAX_CACHE_SIZE` in `draw-glyph.c`), so purging also keeps a full
page of CJK from holding that megabyte across pages.

## Fallback ladder (WhiteOS `bands` equivalent)

1. `full-page` at 100 %
2. `full-page` at 90 %
3. `full-page` at 80 % with the image budget reduced to 96 KB
4. the same at 80 % with antialiasing off and a 64 KB image budget
5. `tiles` at 100 % with 384-row bands (glyph purge after every band)
6. the same with 192-row and then 128-row bands
7. `text-only` (images and image masks skipped) at 100 %, 80 %, 60 %

Every attempt starts from a rebuilt context; without that each fallback began
with less PSRAM than the one before it (measured 5.0 MB -> 3.5 MB -> 0.4 MB).
The tile buffer (86 KB now, 259 KB with 384-row bands) is allocated lazily, so
the full-page path no longer pays for it.

## Open problem: pages that still fail every step

Pages 19, 41, 44, 58 and 65 still fail every step (18 and 23/75 were fixed by
the glyph purge plus bands). Serial shows:

```
PSRAM realloc 463911 failed: free=328284 largest=319476
[PDF] full-page reason: realloc (463911 bytes) failed
```

`free` is still hundreds of KB, so this is **contiguous-block failure**, not
total exhaustion: MuPDF grows a ~464 KB buffer while the heap is fragmented by
the page's own allocations. Lowering the output scale does not help, which
points at the content stream / glyph work rather than the image budget.

The request sizes seen are 463,911 / 572,206 / 695,866 bytes and they do **not**
move when the image budget or the AA level changes, so they are page content
buffers, not decoded images.

Two experiments were tried and both made things **worse** — do not repeat them:

1. **A 2 MB large-block arena** (bump allocator in `psramMalloc` for >= 96 KB,
   reset with the context). Taking 2 MB out of the same 5.9 MB left the page's
   small allocations only ~3.9 MB; pages 41/58 failed earlier than before.
2. **A bounded MuPDF store** (`fz_new_context(..., 768 KB)` instead of
   `FZ_STORE_UNLIMITED`) so caches would be evicted mid-page. Page 68 dropped
   from "100 %, 5 s" to "only the 80 % no-AA fallback", i.e. a clear regression.

Even `text-only` at **40 %** fails on those pages (page 41: 1.03 MB free
afterwards), so the output size is not what limits them — their content alone
does not fit in the available PSRAM whatever the scale.

What the numbers say instead: these pages need roughly **5 MB of working set**
on their own, and the free PSRAM after opening the document is ~5.8 MB. The
572 KB block is only the last straw; there is no fragmentation trick that
creates memory that is not there. The remaining levers are to shrink MuPDF's
per-page working set (for example a banded pass that also empties the store
between bands, which the tile path already does) or to accept a degraded page
with a clear "PDF 读取失败，可以重试" notice instead of an error screen.

## Logging caveat (cost several rounds)

`LOG_ERR` lines emitted right after a failed `fz_run_page` are sometimes lost in
transit (USB CDC burst); the per-page verdict line uses a raw
`logSerial.printf` on purpose. When a serial capture shows an attempt starting
and then nothing, do not conclude the catch block did not run: add a raw line
or an integer error code (`fz_caught(ctx)`) and re-test.

## Text reflow (the clarity path)

Text-only pages are laid out with the reader font instead of being rasterised.
Verified on the 112-page book: page 41 (previously unrenderable) now shows a
complete, crisp page, and pages 28/44/58/65 reflow too.

Rules, all of them learned on the device:

* **Per document, not per page.** `probeReflowSupport()` samples three pages
  (1, middle, last) at open and enables reflow only when at least half of them
  yield >= 400 bytes of text and draw no image. Some PDFs expose only headers
  and page numbers because the body font has no Unicode mapping (page 40 of the
  test book: 96 bytes), and reflowing those would drop the whole body.
* **Mixed text+figure pages keep the bitmap** — reflow would silently lose the
  figure. Only when the bitmap render itself failed does the reader retry the
  page as reflowed text (figures missing, text readable).
* **A page is never replaced by a nearly empty reflow**: under 200 bytes of
  extracted text the layout is rejected and the bitmap path runs instead.
* With the reader's Hyphenation setting enabled, ASCII words are broken with
  the bundled English Liang patterns at a fitting syllable boundary and get a
  visible `-`; tries are kept resident because PDF load runs on PSRAM-stacked
  tasks and cannot read internal flash for them.
* Wrap at `usableWidth - 16`; `getTextWidth` ignores the last glyph's right
  bearing and the rotated draw clips at the panel edge (long lines lost their
  final character at exactly `usableWidth`).
* On a reflow-enabled book the background BMP cache is skipped: each reflowed
  page would otherwise cost ~13 s of rendering that is never displayed.

## Verified coverage (112-page test book)

Sampled pages on separate cold boots, every page reported:

`10 20 30 40 50 60 70 80 90 100 110` -> **11/11 usable**, 9 reflowed
(25-72 lines each), 2 bitmaps (40, 60). Earlier targeted runs add 1, 2, 7, 33,
49, 68, 28, 41, 44, 58, 65 (reflow) and 6, 23, 75 (text-only bitmap).

**Historical note:** page 19 used to be the only page that still failed: it was
a mixed text+figure page whose body had no visible Unicode mapping, so the old
reflow probe skipped it and the bitmap path needed a contiguous block it never
got.  With the final extraction/typography pipeline the page now yields 57
lines and reflows in three screens.  Page 60 remains a bitmap page because it
has only 75 bytes of text.

Full-book automation was attempted three ways and none is reliable yet: one
burst of PDFNEXT drops commands in the UART buffer; per-page waits exceed
30 minutes because failing pages take 50-70 s; jumping with PDFBIG stacks one
Activity (and one MuPDF document) per jump until memory runs out. The serial
self-test command added for this hangs on page 1 and needs the worker/UI render
overlap fixed before it can be trusted.

## Final coverage (112-page book, every page on its own cold boot)

```
historical sweep: reflow 99 · bitmap 12 · failed 1 (page 19)
final targeted recheck: page 19 and page 41 reflow; page 60 bitmap
```

The final targeted recheck is the one that matches the delivered XIP module.
Page 19/41 reflow after the out-of-core font and width-scaled typography
changes; page 60 stays bitmap.  The 5-page image PDF renders every page through
the bitmap path, now with the short-text/vector ink-bbox crop used for scans.

The verification harness is `/tmp/pdf_verify_all.sh` (not in the repo): reset the
board, open the book at page N, read its verdict, write one line, next page. It
is deliberately independent per page, because every faster scheme lost serial
lines or exhausted PSRAM by stacking activities.

## SD code-paging feasibility probe (2026-11-17): gate failed

Goal: keep MuPDF out of app0 and load its code from SD at runtime. A minimal
probe was built (`src/pdf/PdfPagingProbe.cpp`, env `readpico_paging_probe`,
serial `CMD:PGPROBE`) to test ESP32-S3 MMU executable mappings and an
ITLB-miss/instruction-fetch exception handler.

Measured on Read Pico:

- `heap_caps_aligned_alloc(SPIRAM)` + `esp_mmu_map(..., MMU_MEM_CAP_EXEC)`
  returns an executable IROM virtual address; a tiny Xtensa function in the
  mapped PSRAM page executes correctly (`result=42`).
- `mmu_hal_unmap_region` does not invalidate the cached ITLB entry: the same
  function still executes after unmapping.
- Calling a never-mapped IROM address does **not** reach an installed
  `xt_set_exception_handler` for `EXCCAUSE_ITLB_MISS`, `INSTR_ERROR`,
  `INSTR_ADDR_ERROR` or `LOAD_STORE_ADDR_ERROR`; it panics and reboots.

The implemented alternative is documented in [`pdf-xip.md`](pdf-xip.md): the
MuPDF engine is linked at fixed addresses, stored in the dedicated `pdfmod`
partition, and executed from flash while mutable data lives in PSRAM via an
MMU-mapped alias.
Conclusion: MMU demand paging on this IDF/Xtensa port is not a usable path;
the exception table cannot recover code-page misses. The remaining SD option
is a module/GOT trampoline overlay (plan B), which requires splitting MuPDF
into pageable modules and indirect calls. That is a research project and does
not yet meet the full-page acceptance gate. Full MuPDF still needs flash XIP
(app or a dedicated capability partition); no PDF feature was enabled by this
probe. The probe is opt-in only and is not linked into normal `readpico` builds.
